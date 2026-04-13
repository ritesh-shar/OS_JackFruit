/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * FIXED VERSION:
 * - restores proper function bodies / braces
 * - adds safer timer callback structure
 * - avoids doing expensive process/mm work while holding spinlock
 * - checks duplicate registration
 * - validates register/unregister arguments
 * - cleans up list safely on module unload
 * - keeps the char device interface compatible with monitor_ioctl.h
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

struct container_resource_node {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    int soft_limit_warned;
    struct list_head node;
};

static LIST_HEAD(monitored_containers);
static DEFINE_SPINLOCK(monitored_list_lock);

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* FIX: helper to find a registered container by (pid, container_id) */
static struct container_resource_node *
find_node_locked(pid_t pid, const char *container_id)
{
    struct container_resource_node *entry;

    list_for_each_entry(entry, &monitored_containers, node) {
        if (entry->pid == pid &&
            strncmp(entry->container_id, container_id, MONITOR_NAME_LEN) == 0)
            return entry;
    }

    return NULL;
}

/* FIX: restore valid function body and return paths */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;
    long rss_bytes = -1;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }

    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        rss_bytes = rss_pages * PAGE_SIZE;
        mmput(mm);
    }

    put_task_struct(task);
    return rss_bytes;
}

static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    pr_warn("[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
            container_id, pid, rss_bytes, limit_bytes);
}

/* FIX: keep kill helper small and self-contained */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    pr_warn("[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
            container_id, pid, rss_bytes, limit_bytes);
}

/*
 * FIX: do not call get_rss_bytes()/kill_process() while holding the spinlock.
 * Strategy:
 * 1) take lock, copy minimal node info into a temporary array
 * 2) drop lock
 * 3) inspect tasks / rss and decide action
 * 4) re-lock only to mutate the monitored list
 *
 * This avoids expensive mm/task work under spin_lock_irqsave().
 */
struct monitor_snapshot {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit;
    unsigned long hard_limit;
    int soft_limit_warned;
};

static void timer_callback(struct timer_list *t)
{
    struct container_resource_node *entry;
    struct monitor_snapshot *snapshots = NULL;
    unsigned long flags;
    size_t count = 0, idx = 0, i;

    (void)t;

    /* First pass: count list size under lock */
    spin_lock_irqsave(&monitored_list_lock, flags);
    list_for_each_entry(entry, &monitored_containers, node) {
        count++;
    }
    spin_unlock_irqrestore(&monitored_list_lock, flags);

    if (count == 0) {
        mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
        return;
    }

    snapshots = kcalloc(count, sizeof(*snapshots), GFP_ATOMIC);
    if (!snapshots) {
        pr_err("[container_monitor] snapshot allocation failed\n");
        mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
        return;
    }

    /* Second pass: copy state under lock */
    spin_lock_irqsave(&monitored_list_lock, flags);
    list_for_each_entry(entry, &monitored_containers, node) {
        if (idx >= count)
            break;
        snapshots[idx].pid = entry->pid;
        snapshots[idx].soft_limit = entry->soft_limit;
        snapshots[idx].hard_limit = entry->hard_limit;
        snapshots[idx].soft_limit_warned = entry->soft_limit_warned;
        strscpy(snapshots[idx].container_id, entry->container_id, MONITOR_NAME_LEN);
        idx++;
    }
    spin_unlock_irqrestore(&monitored_list_lock, flags);

    for (i = 0; i < idx; i++) {
        long rss = get_rss_bytes(snapshots[i].pid);

        if (rss < 0) {
            /* FIX: process disappeared; remove registration cleanly */
            spin_lock_irqsave(&monitored_list_lock, flags);
            entry = find_node_locked(snapshots[i].pid, snapshots[i].container_id);
            if (entry) {
                list_del(&entry->node);
                kfree(entry);
            }
            spin_unlock_irqrestore(&monitored_list_lock, flags);
            continue;
        }

        if ((unsigned long)rss >= snapshots[i].hard_limit) {
            kill_process(snapshots[i].container_id,
                         snapshots[i].pid,
                         snapshots[i].hard_limit,
                         rss);

            /* FIX: after hard limit kill, remove node from monitor list */
            spin_lock_irqsave(&monitored_list_lock, flags);
            entry = find_node_locked(snapshots[i].pid, snapshots[i].container_id);
            if (entry) {
                list_del(&entry->node);
                kfree(entry);
            }
            spin_unlock_irqrestore(&monitored_list_lock, flags);
            continue;
        }

        if (!snapshots[i].soft_limit_warned &&
            (unsigned long)rss >= snapshots[i].soft_limit) {
            log_soft_limit_event(snapshots[i].container_id,
                                 snapshots[i].pid,
                                 snapshots[i].soft_limit,
                                 rss);

            /* FIX: mark warning only under lock after re-finding node */
            spin_lock_irqsave(&monitored_list_lock, flags);
            entry = find_node_locked(snapshots[i].pid, snapshots[i].container_id);
            if (entry)
                entry->soft_limit_warned = 1;
            spin_unlock_irqrestore(&monitored_list_lock, flags);
        }
    }

    kfree(snapshots);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;
    struct container_resource_node *new_node;
    struct container_resource_node *entry;
    unsigned long flags;
    bool found = false;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* FIX: basic pid/container-id validation */
    if (req.pid <= 0)
        return -EINVAL;

    if (req.container_id[0] == '\0')
        return -EINVAL;

    if (cmd == MONITOR_REGISTER) {
        pr_info("[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
                req.container_id, req.pid,
                req.soft_limit_bytes, req.hard_limit_bytes);

        if (req.soft_limit_bytes == 0 || req.hard_limit_bytes == 0 ||
            req.soft_limit_bytes >= req.hard_limit_bytes) {
            pr_warn("[container_monitor] Invalid limits for PID %d\n", req.pid);
            return -EINVAL;
        }

        new_node = kzalloc(sizeof(*new_node), GFP_KERNEL);
        if (!new_node)
            return -ENOMEM;

        new_node->pid = req.pid;
        new_node->soft_limit = req.soft_limit_bytes;
        new_node->hard_limit = req.hard_limit_bytes;
        new_node->soft_limit_warned = 0;
        strscpy(new_node->container_id, req.container_id, MONITOR_NAME_LEN);
        INIT_LIST_HEAD(&new_node->node);

        spin_lock_irqsave(&monitored_list_lock, flags);

        /* FIX: reject duplicate registration of same (pid, container_id) */
        entry = find_node_locked(req.pid, req.container_id);
        if (entry) {
            spin_unlock_irqrestore(&monitored_list_lock, flags);
            kfree(new_node);
            return -EEXIST;
        }

        list_add_tail(&new_node->node, &monitored_containers);
        spin_unlock_irqrestore(&monitored_list_lock, flags);
        return 0;
    }

    /* MONITOR_UNREGISTER path */
    pr_info("[container_monitor] Unregister request container=%s pid=%d\n",
            req.container_id, req.pid);

    spin_lock_irqsave(&monitored_list_lock, flags);
    entry = find_node_locked(req.pid, req.container_id);
    if (entry) {
        list_del(&entry->node);
        kfree(entry);
        found = true;
    }
    spin_unlock_irqrestore(&monitored_list_lock, flags);

    return found ? 0 : -ENOENT;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
#ifdef CONFIG_COMPAT
    .compat_ioctl = monitor_ioctl,
#endif
};

static int __init monitor_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0)
        return ret;

    cdev_init(&c_dev, &fops);
    c_dev.owner = THIS_MODULE;

    ret = cdev_add(&c_dev, dev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        ret = PTR_ERR(cl);
        cdev_del(&c_dev);
        unregister_chrdev_region(dev_num, 1);
        return ret;
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        cdev_del(&c_dev);
        unregister_chrdev_region(dev_num, 1);
        return -EINVAL;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    pr_info("[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

static void __exit monitor_exit(void)
{
    struct container_resource_node *entry, *tmp;
    unsigned long flags;

    /* FIX: stop timer first so no new callback races with teardown */
    timer_delete_sync(&monitor_timer);

    /* FIX: free list before device/class teardown */
    spin_lock_irqsave(&monitored_list_lock, flags);
    list_for_each_entry_safe(entry, tmp, &monitored_containers, node) {
        list_del(&entry->node);
        kfree(entry);
    }
    spin_unlock_irqrestore(&monitored_list_lock, flags);

    device_destroy(cl, dev_num);
    class_destroy(cl);
    cdev_del(&c_dev);
    unregister_chrdev_region(dev_num, 1);

    pr_info("[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Student");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
