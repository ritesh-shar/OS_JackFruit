/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */


#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include "monitor_ioctl.h"


#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)


typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;


typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;


/* FIX 1: Add stop_kind_t enum before container_record_t */
typedef enum {
    STOP_NONE,
    STOP_GRACEFUL,
    STOP_FORCED
} stop_kind_t;


/* FIX 2 & 3: Added waiting_cli_fd and termination fields */
typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int waiting_cli_fd;
    stop_kind_t termination;
    void *child_stack;              /* NEW: stack allocated for clone child */
    struct container_record *next;
} container_record_t;


typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;


typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;


typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;


typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;


typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;


typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;


static volatile sig_atomic_t global_shutdown_flag = 0;
static volatile sig_atomic_t child_exit_flag = 0;

typedef struct {
    supervisor_ctx_t *ctx;
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
} log_reader_arg_t;

static void *log_reader_thread(void *arg)
{
    log_reader_arg_t *a = (log_reader_arg_t *)arg;
    log_item_t item;
    ssize_t nread;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, a->container_id, sizeof(item.container_id) - 1);

    while ((nread = read(a->read_fd, item.data, sizeof(item.data))) > 0) {
        item.length = (size_t)nread;
        if (bounded_buffer_push(&a->ctx->log_buffer, &item) != 0) {
            break;
        }

        memset(&item, 0, sizeof(item));
        strncpy(item.container_id, a->container_id, sizeof(item.container_id) - 1);
    }

    close(a->read_fd);
    free(a);
    return NULL;
}


int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;


    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);


    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;


    return 0;
}


int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;


    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);


    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;


    return 0;
}

static void sig_handler(int signo)
{
    (void)signo;
    global_shutdown_flag = 1;
}


static void sigchld_handler(int signo)
{
    (void)signo;
    child_exit_flag = 1;
}


static container_record_t *find_container_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *cur = ctx->containers;
    while (cur) {
        if (strncmp(cur->id, id, sizeof(cur->id)) == 0)
            return cur;
        cur = cur->next;
    }
    return NULL;
}

static void free_command_argv(char **argv)
{
    size_t i;
    if (!argv)
        return;
    for (i = 0; argv[i] != NULL; i++)
        free(argv[i]);
    free(argv);
}

static int read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = read(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t)n;
    }

    return 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t total = 0;

    while (total < len) {
        ssize_t n = write(fd, p + total, len - total);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        total += (size_t)n;
    }

    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor \n"
            "  %s start <id> <rootfs> <command>\n"
            "  %s run <id> <rootfs> <command>\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}


static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}


static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;


    memset(buffer, 0, sizeof(*buffer));


    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;


    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }


    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }


    return 0;
}


static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}


static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}


/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->head] = *item;
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}


/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }
    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }
    *item = buffer->items[buffer->tail];
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}


/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
/* FIX 5: closing brace was misplaced inside the while loop — moved to after return NULL */
void *logging_thread(void *arg)
{
    bounded_buffer_t *buffer = (bounded_buffer_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    FILE *logfile;


    mkdir(LOG_DIR, 0755);
    while (bounded_buffer_pop(buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        logfile = fopen(path, "a");
        if (logfile) {
            fwrite(item.data, 1, item.length, logfile);
            fclose(logfile);
        } else {
            fprintf(stderr, "Failed to open log: %s\n", path);
        }
    }

    printf("[logging_thread] Shutdown complete. Buffer drained.\n");
    return NULL;
}


/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;
    char **cmd_argv = NULL;
    int rc = 1;
    int proc_mounted = 0;

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount --make-rprivate /");
        goto out;
    }

    if (sethostname(config->id, strlen(config->id)) != 0) {
        perror("sethostname");
        goto out;
    }
    errno = 0;
    if (config->nice_value != 0) {
        if (nice(config->nice_value) == -1 && errno != 0) {
            perror("nice");
            goto out;
        }
    }

    cmd_argv = calloc(4, sizeof(char *));
    if (!cmd_argv) {
        perror("calloc");
        goto out;
    }

    cmd_argv[0] = strdup("/bin/sh");
    cmd_argv[1] = strdup("-c");
    cmd_argv[2] = strdup(config->command);
    cmd_argv[3] = NULL;

    if (!cmd_argv[0] || !cmd_argv[1] || !cmd_argv[2]) {
        fprintf(stderr, "command allocation failed\n");
        goto out;
    }

    if (dup2(config->log_write_fd, STDOUT_FILENO) < 0) {
        perror("dup2 stdout");
        goto out;
    }

    if (dup2(config->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2 stderr");
        goto out;
    }

    close(config->log_write_fd);
    config->log_write_fd = -1;

    if (chroot(config->rootfs) != 0 || chdir("/") != 0) {
        perror("chroot/chdir");
        goto out;
    }

    if (mkdir("/proc", 0555) != 0 && errno != EEXIST) {
        perror("mkdir /proc");
        goto out;
    }
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount proc");
        goto out;
    }
    proc_mounted = 1;

    execv("/bin/sh", cmd_argv);
    perror("execvp");

out:
    if (proc_mounted)
        umount2("/proc", MNT_DETACH);

    if (config) {
        if (config->log_write_fd >= 0)
            close(config->log_write_fd);
        free(config);
    }
    free_command_argv(cmd_argv);
    return rc;
}

static int handle_control_request(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;

    memset(&resp, 0, sizeof(resp));
    if (read_all(client_fd, &req, sizeof(req)) != 0)
        return 1;

    resp.status = 0;

    switch (req.kind) {
    case CMD_START:
    case CMD_RUN: {
        container_record_t *rec = NULL;
        child_config_t *cfg = NULL;
        void *stack = NULL;
        pid_t pid;
        int pipefd[2] = { -1, -1 };
        pthread_t log_tid;
        log_reader_arg_t *lr = NULL;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, req.container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (rec) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "container %s already exists", req.container_id);
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        if (pipe(pipefd) != 0) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "pipe failed: %s", strerror(errno));
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        cfg = calloc(1, sizeof(*cfg));
        if (!cfg) {
            close(pipefd[0]);
            close(pipefd[1]);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        strncpy(cfg->id, req.container_id, sizeof(cfg->id) - 1);
        strncpy(cfg->rootfs, req.rootfs, sizeof(cfg->rootfs) - 1);
        strncpy(cfg->command, req.command, sizeof(cfg->command) - 1);
        cfg->nice_value = req.nice_value;
        cfg->log_write_fd = pipefd[1];

        stack = malloc(STACK_SIZE);
        if (!stack) {
            close(pipefd[0]);
            close(pipefd[1]);
            free(cfg);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        pid = clone(child_fn,
                    (char *)stack + STACK_SIZE,
                    CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                    cfg);

        close(pipefd[1]);
        pipefd[1] = -1;

        if (pid < 0) {
            close(pipefd[0]);
            if (cfg->log_write_fd >= 0)
                close(cfg->log_write_fd);
            free(cfg);
            free(stack);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "clone failed: %s", strerror(errno));
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        lr = calloc(1, sizeof(*lr));
        if (!lr) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(pipefd[0]);
            free(stack);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        lr->ctx = ctx;
        lr->read_fd = pipefd[0];
        strncpy(lr->container_id, req.container_id, sizeof(lr->container_id) - 1);

        if (pthread_create(&log_tid, NULL, log_reader_thread, lr) != 0) {
            free(lr);
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            close(pipefd[0]);
            free(stack);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "log reader thread failed");
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }
        pthread_detach(log_tid);

        rec = calloc(1, sizeof(*rec));
        if (!rec) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            free(stack);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message), "out of memory");
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        strncpy(rec->id, req.container_id, sizeof(rec->id) - 1);
        rec->host_pid = pid;
        rec->started_at = time(NULL);
        rec->state = CONTAINER_STARTING;
        rec->soft_limit_bytes = req.soft_limit_bytes;
        rec->hard_limit_bytes = req.hard_limit_bytes;
        rec->waiting_cli_fd = -1;
        rec->termination = STOP_NONE;
        rec->child_stack = stack;
        snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);

        mkdir(LOG_DIR, 0755);
        {
            int logfd = open(rec->log_path, O_CREAT | O_WRONLY | O_APPEND, 0644);
            if (logfd >= 0)
                close(logfd);
            else
                perror("open log file");
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        rec->next = ctx->containers;
        ctx->containers = rec;
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (register_with_monitor(ctx->monitor_fd, rec->id, rec->host_pid,
                                  rec->soft_limit_bytes,
                                  rec->hard_limit_bytes) != 0) {
            perror("register_with_monitor");
        }

        pthread_mutex_lock(&ctx->metadata_lock);
        if (rec->state == CONTAINER_STARTING)
            rec->state = CONTAINER_RUNNING;

        if (req.kind == CMD_RUN)
            rec->waiting_cli_fd = client_fd;
        pthread_mutex_unlock(&ctx->metadata_lock);

        snprintf(resp.message, sizeof(resp.message),
                 "%s: started pid=%d", rec->id, rec->host_pid);

        if (write_all(client_fd, &resp, sizeof(resp)) != 0) {
            if (req.kind == CMD_RUN) {
                pthread_mutex_lock(&ctx->metadata_lock);
                if (rec->waiting_cli_fd == client_fd)
                    rec->waiting_cli_fd = -1;
                pthread_mutex_unlock(&ctx->metadata_lock);
            }
            return 1;
        }

        if (req.kind == CMD_RUN)
            return 0;

        return 1;
    }

    case CMD_STOP: {
        container_record_t *rec;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, req.container_id);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "container %s not found", req.container_id);
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        if (kill(rec->host_pid, SIGTERM) != 0) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "failed to stop %s: %s", rec->id, strerror(errno));
        } else {
            rec->termination = STOP_GRACEFUL;
            snprintf(resp.message, sizeof(resp.message),
                     "stop requested for %s", rec->id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        (void)write_all(client_fd, &resp, sizeof(resp));
        return 1;
    }

    case CMD_LOGS: {
        char path[PATH_MAX];
        FILE *f;
        char line[CONTROL_MESSAGE_LEN];

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        f = fopen(path, "r");
        if (!f) {
            resp.status = 1;
            snprintf(resp.message, sizeof(resp.message),
                     "log file missing for %s", req.container_id);
            (void)write_all(client_fd, &resp, sizeof(resp));
            return 1;
        }

        while (fgets(line, sizeof(line), f) != NULL) {
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            strncpy(resp.message, line, sizeof(resp.message) - 1);
            if (write_all(client_fd, &resp, sizeof(resp)) != 0)
                break;
        }
        fclose(f);
        return 1;
    }

    case CMD_PS: {
        container_record_t *cur;

        pthread_mutex_lock(&ctx->metadata_lock);
        cur = ctx->containers;
        while (cur) {
            memset(&resp, 0, sizeof(resp));
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message),
                     "%s pid=%d state=%s limits=%lu/%luMB exit=%d sig=%d",
                     cur->id,
                     cur->host_pid,
                     state_to_string(cur->state),
                     cur->soft_limit_bytes / (1024 * 1024),
                     cur->hard_limit_bytes / (1024 * 1024),
                     cur->exit_code,
                     cur->exit_signal);
            if (write_all(client_fd, &resp, sizeof(resp)) != 0)
                break;
            cur = cur->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        return 1;
    }

    default:
        resp.status = 1;
        snprintf(resp.message, sizeof(resp.message),
                 "unsupported command kind=%d", req.kind);
        (void)write_all(client_fd, &resp, sizeof(resp));
        return 1;
    }
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int st;
    pid_t pid;

    while ((pid = waitpid(-1, &st, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);

        container_record_t *cur = ctx->containers;
        while (cur) {
            if (cur->host_pid == pid) {
                control_response_t resp;

                if (WIFEXITED(st)) {
                    cur->state = CONTAINER_EXITED;
                    cur->exit_code = WEXITSTATUS(st);
                    cur->exit_signal = 0;
                } else if (WIFSIGNALED(st)) {
                    cur->state = (cur->termination == STOP_GRACEFUL)
                                 ? CONTAINER_STOPPED
                                 : CONTAINER_KILLED;
                    cur->exit_code = -1;
                    cur->exit_signal = WTERMSIG(st);
                }
                if (cur->state == CONTAINER_EXITED ||
                    cur->state == CONTAINER_STOPPED ||
                    cur->state == CONTAINER_KILLED) {
                    cur->termination = STOP_NONE;
                }

                if (ctx->monitor_fd >= 0) {
                    if (unregister_from_monitor(ctx->monitor_fd, cur->id, cur->host_pid) != 0) {
                        perror("unregister_from_monitor");
                    }
                }

                if (cur->waiting_cli_fd >= 0) {
                    memset(&resp, 0, sizeof(resp));
                    resp.status = 0;
                    snprintf(resp.message, sizeof(resp.message),
                             "%s exited (code=%d signal=%d)",
                             cur->id, cur->exit_code, cur->exit_signal);
                    (void)write_all(cur->waiting_cli_fd, &resp, sizeof(resp));
                    close(cur->waiting_cli_fd);
                    cur->waiting_cli_fd = -1;
                }
                if (cur->child_stack) {
                    free(cur->child_stack);
                    cur->child_stack = NULL;
                }

                break;
            }
            cur = cur->next;
        }

        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */

static void free_container_list(supervisor_ctx_t *ctx)
{
    container_record_t *cur;
    container_record_t *next;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (cur) {
        next = cur->next;

        if (cur->waiting_cli_fd >= 0)
            close(cur->waiting_cli_fd);

        if (cur->child_stack) {
            free(cur->child_stack);
            cur->child_stack = NULL;
        }

        free(cur);
        cur = next;
    }
}

static void stop_all_containers(supervisor_ctx_t *ctx)
{
    container_record_t *cur;

    pthread_mutex_lock(&ctx->metadata_lock);
    cur = ctx->containers;
    while (cur) {
        if (cur->state == CONTAINER_RUNNING || cur->state == CONTAINER_STARTING) {
            kill(cur->host_pid, SIGTERM);
            cur->termination = STOP_GRACEFUL;
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);
}

static int run_supervisor(void)
{
    supervisor_ctx_t ctx;
    int status = 0;
    pthread_t logger_tid;
    struct sockaddr_un addr;
    struct sigaction sa_term, sa_chld, sa_pipe;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    ctx.containers = NULL;
    ctx.should_stop = 0;

    if (pthread_mutex_init(&ctx.metadata_lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return 1;
    }

    if (bounded_buffer_init(&ctx.log_buffer) != 0) {
        fprintf(stderr, "bounded_buffer_init failed\n");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0) {
        perror("Failed to open /dev/container_monitor");
        status = 1;
        goto cleanup;
    }

    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        status = 1;
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        status = 1;
        goto cleanup;
    }

    if (chmod(CONTROL_PATH, 0666) < 0) {
        perror("chmod");
        status = 1;
        goto cleanup;
    }

    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen");
        status = 1;
        goto cleanup;
    }

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = sig_handler;
    sigemptyset(&sa_term.sa_mask);
    sa_term.sa_flags = 0;
    if (sigaction(SIGINT, &sa_term, NULL) < 0) {
        perror("sigaction(SIGINT)");
        status = 1;
        goto cleanup;
    }
    if (sigaction(SIGTERM, &sa_term, NULL) < 0) {
        perror("sigaction(SIGTERM)");
        status = 1;
        goto cleanup;
    }

    memset(&sa_pipe, 0, sizeof(sa_pipe));
    sa_pipe.sa_handler = SIG_IGN;
    sigemptyset(&sa_pipe.sa_mask);
    sa_pipe.sa_flags = 0;
    if (sigaction(SIGPIPE, &sa_pipe, NULL) < 0) {
        perror("sigaction(SIGPIPE)");
        status = 1;
        goto cleanup;
    }

    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = sigchld_handler;
    sigemptyset(&sa_chld.sa_mask);
    sa_chld.sa_flags = SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa_chld, NULL) < 0) {
        perror("sigaction(SIGCHLD)");
        status = 1;
        goto cleanup;
    }

    if (pthread_create(&logger_tid, NULL, logging_thread, &ctx.log_buffer) != 0) {
        perror("pthread_create");
        status = 1;
        goto cleanup;
    }

    printf("[supervisor] Entering event loop...\n");

    struct pollfd pfd = { .fd = ctx.server_fd, .events = POLLIN };
    while (!global_shutdown_flag) {
        if (child_exit_flag) {
        child_exit_flag = 0;
        reap_children(&ctx);
    }
        int ret = poll(&pfd, 1, 500);
        if (ret > 0 && (pfd.revents & POLLIN)) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd >= 0) {
            handle_control_request(&ctx, client_fd);
            close(client_fd);
        }
    }
    }

    printf("[supervisor] Shutting down...\n");

    stop_all_containers(&ctx);
    sleep(1);

    reap_children(&ctx);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(logger_tid, NULL);

cleanup:
    if (ctx.server_fd >= 0) {
        close(ctx.server_fd);
        unlink(CONTROL_PATH);
    }

    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    free_container_list(&ctx);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return status;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        switch (errno) {
        case ENOENT:
            fprintf(stderr,
                    "supervisor socket not found at %s "
                    "(is the supervisor running?)\n",
                    CONTROL_PATH);
            break;
        case ECONNREFUSED:
            fprintf(stderr,
                    "supervisor is not accepting connections at %s "
                    "(stale socket or crashed supervisor)\n",
                    CONTROL_PATH);
            break;
        case EACCES:
        case EPERM:
            fprintf(stderr,
                    "permission denied connecting to %s "
                    "(try sudo or fix socket permissions)\n",
                    CONTROL_PATH);
            break;
        default:
            fprintf(stderr,
                    "connect to %s failed: %s\n",
                    CONTROL_PATH, strerror(errno));
            break;
        }
        close(fd);
        return 1;
    }

    if (write_all(fd, req, sizeof(*req)) != 0) {
        perror("write request failed");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_START || req->kind == CMD_STOP) {
        if (read_all(fd, &resp, sizeof(resp)) == 0) {
            if (resp.message[0] != '\0')
                printf("%s\n", resp.message);
        } else {
            perror("read response");
            close(fd);
            return 1;
        }

        close(fd);
        return 0;
    }

    for (;;) {
        if (read_all(fd, &resp, sizeof(resp)) != 0)
            break;

        if (resp.message[0] != '\0')
            printf("%s\n", resp.message);
    }

    close(fd);
    return 0;
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    return send_control_request(&req);
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;


    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <command>\n", argv[0]);
        return 1;
    }


    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;


    return send_control_request(&req);
}


static int cmd_run(int argc, char *argv[])
{
    control_request_t req;


    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <command>\n", argv[0]);
        return 1;
    }


    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;


    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;


    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }


    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);


    return send_control_request(&req);
}


static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;


    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }


    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);


    return send_control_request(&req);
}


int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }


    if (strcmp(argv[1], "supervisor") == 0) {
        return run_supervisor();
    }


    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);


    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);


    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();


    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);


    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);


    usage(argv[0]);
    return 1;
}
