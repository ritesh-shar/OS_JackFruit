## Supervised Multi-Container Runtime

### Team Members
 ## 1- Rithish P M. SRN - PES2UG24CS405.
 ## 2- Ritesh Kumar Sharma. SRN - PES2UG24CS404.

### 1. Build & Run Instructions

## Prerequisites
# Ubuntu 22.04/24.04
# build-essential, linux-headers-$(uname -r)
# A valid rootfs (instructions below

## Setup
# 1) Prepare rootfs
``` bash
mkdir rootfs
wget https://dl-cdn.alpinelinux.org/alpine/v3.19/releases/x86_64/alpine-minirootfs-3.19.1-x86_64.tar.gz
tar -xzf alpine-minirootfs-*.tar.gz -C rootfs/
```

# 2) Build and Load
```bash
make
sudo insmod monitor.ko
```

# 3) Start Supervisor
```bash
sudo ./engine supervisor ./rootfs
```

# 4) CLI Commands
```bash
./engine run c1 ./rootfs "ls -R /"
./engine ps
./engine stop c1
```

### 2. Engineering Analysis

## I. Isolation Mechanisms:
This runtime utilizes Linux Namespaces to create isolated environments. We specifically use:
1)CLONE_NEWPID: Ensures the container has its own process tree; the child process cannot see or kill host processes.
2)CLONE_NEWNS: Isolates mount points. Combined with chroot(), this prevents the container from accessing the host filesystem.
3)CLONE_NEWUTS: Allows the container to have a unique hostname.
Fundamental Concept: While isolated, the container still shares the Host Kernel. This means the kernel's syscall interface is the primary attack surface.

## II. Process Lifecycle & Supervisor
The Supervisor acts as the dedicated parent (Init process) for all containers.
1) Metadata Tracking: It maintains a synchronized linked list of all active containers.
2) Zombie Reaping: Using a poll() loop, the supervisor periodically calls waitpid() with WNOHANG.
   This ensures that even if a container exits instantly (like ls), its resources are reaped and no <defunct> processes remain in the OS process table.

## III. IPC & Synchronization
We implemented two distinct IPC mechanisms to separate the Control Plane from the Data Plane:
1)Unix Domain Socket: Used for CLI-to-Supervisor commands. It is protected by a metadata_lock (Mutex) to ensure that two CLI instances don't corrupt the container list simultaneously.
2)Pipes & Bounded Buffer: Used for logging. A Producer-Consumer model is used where worker threads push logs into a buffer, and a dedicated logging thread writes them to disk.
3)Synchronization: We use pthread_cond_t (Condition Variables) to block the logging thread when the buffer is empty, preventing high CPU usage from busy-waiting.

## IV. Memory Management
The Kernel Monitor (LKM) tracks Resident Set Size (RSS) via IOCTLs.
1) RSS vs Virtual Memory: RSS only counts pages currently in physical RAM.
2) Enforcement: Hard limits are enforced in kernel space because the kernel is the only entity that can reliably preempt a process or trigger an OOM (Out Of Memory) event before the entire system crashes.

### 3. Design Decisions & Tradeoffs:

|Component|Choice|Tradeoff|Justification|
|---------|------|--------|-------------|
|Supervisor|Long-running daemon|Increased memory overhead on host|Necessary for persistent logging and reliable zombie reaping|
|IPC|Unix Domain Socket|More complex than FIFOs|Provides a reliable, bi-directional stream for complex control structures|
|Logging|Asynchronous Threads|Potential for log loss on crash|Prevents the container execution from slowing down due to slow Disk I/O|

### 4. Screenshots:
# IPC Proof:
# Zombie Prevention:
# Signal Handling:
# Orderly Shutdown:
