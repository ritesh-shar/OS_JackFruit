#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

/*
 * Shared ioctl interface between:
 * - engine.c (user space runtime / supervisor)
 * - monitor.c (kernel module)
 */

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <sys/ioctl.h>
#include <sys/types.h>
#endif

#define MONITOR_NAME_LEN 32

/*
 * Single request structure used for both register and unregister.
 * For unregister, only pid + container_id are required.
 */
struct monitor_request {
    pid_t pid;
    char container_id[MONITOR_NAME_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
};

/*
 * Ioctl magic chosen arbitrarily for this module.
 * Keep it identical in user space and kernel space.
 */
#define MONITOR_IOCTL_MAGIC 'M'

#define MONITOR_REGISTER   _IOW(MONITOR_IOCTL_MAGIC, 1, struct monitor_request)
#define MONITOR_UNREGISTER _IOW(MONITOR_IOCTL_MAGIC, 2, struct monitor_request)

#endif /* MONITOR_IOCTL_H */
