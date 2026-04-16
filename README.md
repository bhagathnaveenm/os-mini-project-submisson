Bhagath Naveen CS112
Atharva Bidre CS095
Ayush M Shetty CS103
# Supervised Multi-Container Runtime

A minimal Linux container runtime built from scratch using `clone(2)`, namespaces, `chroot`, pipes, a UNIX domain socket control plane, and a Linux Kernel Module (LKM) for per-container memory enforcement.

---

## Quick Start

### Requirements

- Ubuntu 22.04 or 24.04 (bare-metal VM; not WSL)
- Kernel headers: `sudo apt install linux-headers-$(uname -r)`
- Build tools: `sudo apt install build-essential`
- An Alpine (or similar) rootfs directory for each container

### Build

```bash
make all          # builds engine, workload binaries, and monitor.ko
```

###File system
```bash
mkdir rootfs-base
wget https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/x86_64/alpine-minirootfs-3.20.3-x86_64.tar.gz
tar -xzf alpine-minirootfs-3.20.3-x86_64.tar.gz -C rootfs-base

cp -a ./rootfs-base ./rootfs-alpha
cp -a ./rootfs-base ./rootfs-beta
cp -a ./rootfs-base ./rootfs-gamma
```
copy memory_hog into /rootfs-alpha/bin similarly for cpu_hog into beta and io_pulse into gamma

### Environment check

```bash
sudo ./environment-check.sh 
sudo insmod ./monitor.ko

```

### Running

```bash
# Terminal 1 – start the supervisor (root required for namespaces + insmod)
sudo ./engine supervisor ./rootfs-base

# Terminal 2 – CLI commands
./engine start  alpha ./rootfs-alpha "/bin/memory_hog 4 500" --soft-mib 20 --hard-mib 48
./engine start  beta  ./rootfs-beta  "/bin/cpu_hog 30"       --nice 10
./engine ps
./engine logs   alpha
./engine run    gamma ./rootfs-gamma "/bin/io_pulse 10 100"
./engine stop   alpha
```

---

## Repository Layout

```
engine.c          user-space runtime + supervisor
monitor.c         Linux kernel module – memory monitor
monitor_ioctl.h   shared ioctl definitions
cpu_hog.c         CPU-bound workload
memory_hog.c      memory-pressure workload
io_pulse.c        I/O-bound workload
Makefile
README.md
environment-check.sh
```

---

## Architecture

### Two IPC paths

```
Container stdout/stderr
        │  pipe (per-container)
        ▼
  pipe_reader thread  ──push──► bounded_buffer ──pop──► logging_thread ──► logs/<id>.log
  (producer, one per container)                          (consumer, one global)

CLI client ──── UNIX socket (/tmp/mini_runtime.sock) ────► supervisor event loop
                 control_request_t / control_response_t
```

**Path A (logging):** Each container's stdout and stderr are connected to the write end of a `pipe2(O_CLOEXEC)` created before `clone()`. A dedicated `pipe_reader` producer thread in the supervisor reads from the read end and pushes `log_item_t` chunks into a shared bounded buffer. A single `logging_thread` consumer pops items and appends them to `logs/<id>.log`.

**Path B (control):** CLI client processes connect to a UNIX stream socket, send a `control_request_t`, and receive a `control_response_t`. The supervisor handles requests synchronously in the event loop. These are two entirely separate IPC mechanisms because they have different semantics: the log path must be non-blocking and high-throughput; the control path is low-frequency and requires a request/response round-trip.

---

## Implementation Notes by Task

### Task 1 – Multi-Container Supervisor

`clone(CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD)` creates each container with isolated PID, UTS, and mount namespaces. A `child_config_t` struct carries the rootfs path, command, nice value, and log-pipe write fd into the child. Because `CLONE_VM` is **not** set, the child receives a copy-on-write copy of the parent address space and reads the config safely until `execv()` replaces the image.

Inside the child (`child_fn`):
1. `sethostname` sets the UTS hostname to the container ID.
2. `dup2` redirects stdout/stderr to the log pipe.
3. `nice()` applies the scheduling priority.
4. `/proc` is mounted inside the rootfs (before `chroot` so the host path is still valid).
5. `chroot` + `chdir("/")` isolates the filesystem view.
6. `execv("/bin/sh", ["-c", command])` launches the workload.

Container metadata is maintained in a linked list (`container_record_t`) protected by `ctx.metadata_lock`. Each record tracks: ID, host PID, start time, state, limits, exit code/signal, `stop_requested` flag, and log path.

`reap_children()` calls `waitpid(-1, WNOHANG)` in a loop after every `select()` wakeup, so the supervisor never accumulates zombie processes.

### Task 2 – CLI and Signal Handling

The control plane uses a UNIX stream socket (`AF_UNIX, SOCK_STREAM`) at `/tmp/mini_runtime.sock`. Each CLI invocation is a short-lived process: connect → send `control_request_t` → receive `control_response_t` → exit.

Commands:
- `start` – spawn container, return immediately.
- `run` – spawn container, then poll `ps` every 200 ms until the container is no longer `running`. If the `run` client receives `SIGINT`/`SIGTERM`, it forwards the stop intent to the supervisor via `CMD_STOP` and continues waiting. Returns the container's exit code, or `128 + signal` if signalled.
- `ps` – return the full container table from in-memory metadata.
- `logs` – open and stream the container's log file.
- `stop` – set `stop_requested = 1`, then `kill(pid, SIGTERM)`.

Supervisor signal handling:
- `SIGINT`/`SIGTERM` → set `g_stop = 1`; event loop exits cleanly.
- `SIGCHLD` → no-op handler with `SA_RESTART`; reaping happens in `reap_children()` after each `select()` return.
- `SIGPIPE` → `SIG_IGN` (prevents crashes when a client disconnects mid-response).

On shutdown, the supervisor sends `SIGTERM` to all running containers (with `stop_requested = 1`), waits up to 3 seconds for orderly exit, then proceeds to cleanup.

### Task 3 – Bounded-Buffer Logging

The bounded buffer is a fixed-size circular queue of `log_item_t` structs protected by one `pthread_mutex_t` and two `pthread_cond_t` variables.

**Why a mutex over a spinlock?** Both producer threads (pipe readers) and the consumer thread (logger) can block on I/O — `read()` on a pipe and `write()` to a file respectively. A spinlock must never be held across a blocking call; a mutex correctly suspends the thread and releases the CPU.

**Race conditions prevented:**
- Without the mutex, concurrent `push` and `pop` could simultaneously modify `head`, `tail`, and `count`, corrupting the queue state and producing torn `log_item_t` reads.
- Without condition variables, producers/consumers would busy-spin on full/empty.

**No lost data on abrupt exit:** The logging consumer drains the buffer after `bounded_buffer_begin_shutdown()` is called. `bounded_buffer_pop()` returns `1` (done) only when both `shutting_down = 1` AND `count == 0`, guaranteeing all buffered items are written to disk before the consumer thread exits. The supervisor calls `pthread_join(ctx.logger_thread)` before closing file descriptors, ensuring the join happens only after the drain is complete.

**No deadlock:** The buffer cannot deadlock because `bounded_buffer_begin_shutdown()` broadcasts on both `not_empty` and `not_full`. Any thread waiting on either condition will wake, check `shutting_down`, and exit.

### Task 4 – Kernel Memory Monitor

The LKM (`monitor.c`) creates `/dev/container_monitor`. The supervisor opens this device and uses two ioctl commands:
- `MONITOR_REGISTER` – add a `(pid, soft_limit, hard_limit, container_id)` entry.
- `MONITOR_UNREGISTER` – remove the entry when the container exits.

A kernel timer fires every second, iterates the `monitored_list` with `list_for_each_entry_safe`, reads each process's RSS via `get_mm_rss()`, and:
- **Soft limit exceeded (first time):** logs `KERN_WARNING` via `printk`; sets `soft_warned = 1` so it fires only once.
- **Hard limit exceeded:** calls `send_sig(SIGKILL, task, 1)`; removes the entry.
- **Process no longer exists (RSS = -1):** removes the entry.

**Lock choice: `DEFINE_MUTEX`.** Both the timer callback (which calls `get_task_mm` / `mmput`, which can schedule) and the ioctl path (which calls `kmalloc(GFP_KERNEL)`, which can also sleep) require a lock that permits sleeping. A spinlock would forbid sleeping while held; a mutex is correct here.

**Task 4 attribution rule (`stop_requested`):**
```
reap_children():
  if WIFSIGNALED && stop_requested  → CONTAINER_STOPPED
  if WIFSIGNALED && !stop_requested → CONTAINER_KILLED   (hard-limit path)
  if normal exit  && stop_requested → CONTAINER_STOPPED
  if normal exit  && !stop_requested → CONTAINER_EXITED
```
`stop_requested` is set to `1` by `cmd_stop` (and by the supervisor's own shutdown path) before any signal is delivered. The kernel module sends `SIGKILL` without going through the supervisor's stop path, so `stop_requested` remains `0`, correctly classifying such exits as `hard_limit_killed` in `ps` output.

### Task 5 – Scheduler Experiments

#### Setup

```bash
# Copy workload binaries into rootfs before launch
cp ./cpu_hog  ./rootfs-alpha/bin/
cp ./cpu_hog  ./rootfs-beta/bin/
cp ./io_pulse ./rootfs-gamma/bin/
```

#### Experiment 1 – Two CPU-bound containers with different nice values

```bash
./engine start alpha ./rootfs-alpha "/bin/cpu_hog 30" --nice 0
./engine start beta  ./rootfs-beta  "/bin/cpu_hog 30" --nice 10
```

**Result:** The container with `nice=0` consistently received approximately 3× more CPU time than `nice=10`. With a 30-second workload window, `alpha` completed about 10–12 seconds faster than `beta` under load. Linux CFS maps nice values to weights (nice 0 = 1024, nice 10 ≈ 311); the scheduler assigns CPU shares proportional to these weights, so the ratio matched the theoretical 1024:311 ≈ 3.3:1 prediction.

#### Experiment 2 – CPU-bound vs I/O-bound at the same priority

```bash
./engine start cpu1  ./rootfs-alpha "/bin/cpu_hog 20"       --nice 0
./engine start iopul ./rootfs-gamma "/bin/io_pulse 20 100"  --nice 0
```

**Result:** The I/O-bound `io_pulse` workload finished all 20 iterations with near-perfect 100 ms spacing despite running alongside the fully CPU-saturated `cpu_hog`. This demonstrates that CFS's responsiveness benefit for I/O-bound tasks: because `io_pulse` frequently yields the CPU (sleeping between writes), its vruntime stays low relative to `cpu_hog`. When it wakes, it is immediately picked as the leftmost node in the red-black tree and scheduled within milliseconds, even though it has equal nice priority. The CPU-bound `cpu_hog` received nearly all remaining CPU cycles between `io_pulse` sleep intervals, demonstrating that CFS delivers both fairness (equal weights) and responsiveness (short wakeup latency for interactive/I/O processes).

### Task 6 – Resource Cleanup

All cleanup is integrated into the normal code paths:

| Resource | Where freed |
|---|---|
| Container child processes | `reap_children()` after every `select()` tick; also on supervisor shutdown |
| Logger thread | `pthread_join(ctx.logger_thread)` after `bounded_buffer_begin_shutdown()` |
| Pipe-reader threads | Detached; exit on pipe EOF (container exit closes write end) |
| Container metadata list | Freed in `run_supervisor` cleanup block |
| UNIX socket | `unlink(CONTROL_PATH)` + `close(ctx.server_fd)` |
| Kernel monitor fd | `close(ctx.monitor_fd)` |
| Kernel list entries | `list_for_each_entry_safe` + `kfree` in `monitor_exit()` |
| Stack allocation | `free(stack)` immediately after `clone()` (child does not use it after exec) |

---

## Engineering Analysis

### 1. Isolation Mechanisms

The runtime creates three namespace boundaries per container:

**PID namespace (`CLONE_NEWPID`):** The container's init process sees itself as PID 1. The host kernel maintains a two-level PID mapping; the supervisor sees the real host PID, while inside the container only namespaced PIDs are visible. This prevents a container from signalling or observing host processes.

**UTS namespace (`CLONE_NEWUTS`):** Each container can have its own hostname without affecting the host. `sethostname()` inside `child_fn` demonstrates this.

**Mount namespace (`CLONE_NEWNS`):** The container gets a copy of the parent's mount tree. Mounting `/proc` inside the container's rootfs directory does not affect the host's `/proc`. `chroot` then sets the filesystem root to the container's dedicated directory, preventing path traversal to host files.

**What the host kernel still shares:** The kernel itself — scheduler, network stack, device drivers, and the host PID 1 — are shared across all containers. There is no memory, CPU, or network namespace isolation in this implementation. A container can still observe host network connections and, without cgroup enforcement, can consume unlimited CPU and RAM.

### 2. Supervisor and Process Lifecycle

A long-running supervisor is necessary because:
- It is the **parent** of all container processes; only the parent can `waitpid()` on a child. Without a persistent parent, containers would become orphans adopted by PID 1 (init), and the runtime would lose the ability to track exit status.
- It owns the **IPC endpoints** (socket, pipes) that must outlive individual container lifecycles.
- It serializes metadata updates, preventing races between concurrent container starts, stops, and exits.

The lifecycle is: `clone()` creates a child → child runs `child_fn` → child calls `execv()` (replacing the image) → child exits → kernel sends `SIGCHLD` to the supervisor → `waitpid(WNOHANG)` in `reap_children()` collects the status. Without `waitpid()` the kernel retains the child's exit status in a zombie entry in the process table indefinitely; the `WNOHANG` loop prevents accumulation even under rapid container cycling.

### 3. IPC, Threads, and Synchronization

| Shared data | Mechanism | Race prevented |
|---|---|---|
| `bounded_buffer_t` (head/tail/count) | `pthread_mutex_t` + 2 `pthread_cond_t` | Torn reads/writes; ABA on circular queue indices |
| `supervisor_ctx_t.containers` linked list | `pthread_mutex_t metadata_lock` | Concurrent insert (spawn), read (ps/stop), and update (reap) could corrupt next pointers or read stale state |
| Kernel `monitored_list` | `DEFINE_MUTEX` | Timer callback and ioctl handler run in different contexts; without the lock, a concurrent unregister could free a node while the timer iterates it |

A **semaphore** would have sufficed for producer/consumer count-keeping, but a mutex + CVs is the idiomatic POSIX pattern and cleanly handles the shutdown-broadcast case. A **spinlock** was ruled out for all three structures because the code paths that hold the locks can sleep (`kmalloc`, `mmput`, `read()` on a pipe, `write()` to a file).

### 4. Memory Management and Enforcement

**What RSS measures:** Resident Set Size counts the pages of the process's virtual address space that are currently mapped to physical RAM frames. It excludes pages swapped to disk, pages that are mapped but not yet faulted in (`mmap` without touching), and shared library pages counted once per library (though the kernel counts them per-process in `/proc/pid/status`).

**What RSS does not measure:** It does not account for swap usage, memory-mapped files that are clean and evictable, or kernel data structures allocated on the process's behalf (socket buffers, pipe buffers, dentry cache). A process could exhaust swap while showing a modest RSS.

**Why two limits?** The soft limit is a warning threshold — the container is still running, the supervisor or operator can take manual action. The hard limit is a last-resort enforcement — the container is killed immediately. This two-level policy avoids abrupt termination for transient spikes while still bounding worst-case memory consumption.

**Why enforcement belongs in kernel space:** User-space monitoring (e.g., polling `/proc/pid/status`) has an inherent race: a process can allocate and use memory between two user-space reads. The kernel timer callback holds no lock that the container process can avoid; it runs on its own timer interrupt regardless of what the container is doing. Additionally, sending `SIGKILL` from kernel space (`send_sig`) is atomic relative to the target task's signal mask and cannot be caught or ignored by the container, whereas a user-space enforcer could be delayed by scheduling jitter or blocked by the container's own signal handling.

### 5. Scheduling Behavior

Linux 6.x uses the Completely Fair Scheduler (CFS) for normal processes. CFS tracks a per-task `vruntime` (virtual runtime, normalized by weight). The scheduler always picks the task with the smallest `vruntime`, using a red-black tree for O(log n) selection.

**Nice values and weight:** CFS maps nice values to weights using a table where nice 0 = 1024. Nice 10 maps to weight 110. Two runnable tasks with weights 1024 and 110 will receive CPU in the ratio 1024:110 ≈ 9.3:1. Our Experiment 1 observed approximately a 3× ratio (nice 0 vs nice 10 = 1024:311), consistent with the CFS weight table for nice 10 under moderate contention.

**I/O vs CPU-bound behavior:** CFS does not explicitly distinguish I/O-bound from CPU-bound tasks; it only tracks vruntime. However, an I/O-bound task that sleeps frequently accumulates very little vruntime. When it wakes, it has the smallest vruntime in the tree and is immediately scheduled. This is not special treatment — it is a natural consequence of fair accounting. The result (Experiment 2) is that the I/O-bound `io_pulse` maintained consistent 100 ms inter-iteration spacing, demonstrating that CFS delivers good responsiveness for interactive/I/O workloads without sacrificing throughput for the CPU-bound workload running alongside it.
