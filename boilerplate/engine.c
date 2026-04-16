/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Tasks covered:
 *   Task 1 - multi-container supervisor with clone(2) + namespaces + chroot
 *   Task 2 - CLI / control plane over UNIX domain socket; signal handling
 *   Task 3 - bounded-buffer logging pipeline (pipe -> buffer -> log file)
 *   Task 4 - integration with kernel monitor; stop_requested flag for
 *            attribution of termination reason
 *   Task 6 - full resource cleanup on every exit path
 *
 * Key design note on the listening socket:
 *   Only the *listening* fd is O_NONBLOCK so that select()+accept() works
 *   without blocking.  The *accepted* client fd must be left blocking so
 *   that recv(MSG_WAITALL) reliably collects the full request struct.
 *   Previously O_NONBLOCK was set on the listening socket with fcntl()
 *   AFTER listen(); on Linux, accept() inherits O_NONBLOCK from the
 *   listening socket, causing recv() to return EAGAIN immediately and
 *   every command to fail with "bad request size".  Fixed by using
 *   accept4(..., SOCK_CLOEXEC) instead (no SOCK_NONBLOCK) and setting
 *   O_NONBLOCK only on the listening fd via fcntl.
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
#include <unistd.h>

#include "monitor_ioctl.h"

/* ---------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------- */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    32
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MSG_LEN     4096
#define CHILD_COMMAND_LEN   256
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT  (40UL << 20)
#define DEFAULT_HARD_LIMIT  (64UL << 20)
#define DEVICE_NAME         "container_monitor"

/* ---------------------------------------------------------------
 * Enumerations
 * --------------------------------------------------------------- */
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
    CONTAINER_STOPPED,   /* clean stop via `stop` command */
    CONTAINER_KILLED,    /* killed by hard-limit or external SIGKILL */
    CONTAINER_EXITED     /* exited on its own */
} container_state_t;

/* ---------------------------------------------------------------
 * Container metadata record
 * --------------------------------------------------------------- */
typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;
    char              log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

/* ---------------------------------------------------------------
 * Bounded buffer
 * --------------------------------------------------------------- */
typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

/* ---------------------------------------------------------------
 * IPC message types (control plane)
 * --------------------------------------------------------------- */
typedef struct {
    command_kind_t kind;
    char           container_id[CONTAINER_ID_LEN];
    char           rootfs[PATH_MAX];
    char           command[CHILD_COMMAND_LEN];
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    int            nice_value;
} control_request_t;

typedef struct {
    int  status;
    char message[CONTROL_MSG_LEN];
} control_response_t;

/* ---------------------------------------------------------------
 * Child config – read by child before execv() replaces the image
 * --------------------------------------------------------------- */
typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

/* ---------------------------------------------------------------
 * Supervisor context
 * --------------------------------------------------------------- */
typedef struct {
    int              server_fd;   /* listening socket – O_NONBLOCK */
    int              monitor_fd;
    pthread_t        logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t  metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* ---------------------------------------------------------------
 * Global stop flag (set by SIGINT/SIGTERM to supervisor)
 * --------------------------------------------------------------- */
static volatile sig_atomic_t g_stop = 0;

/* ---------------------------------------------------------------
 * Forward declarations
 * --------------------------------------------------------------- */
static void        usage(const char *prog);
static int         parse_mib_flag(const char *, const char *, unsigned long *);
static int         parse_optional_flags(control_request_t *, int, char *[], int);
static const char *state_to_string(container_state_t);
static int         send_control_request(const control_request_t *);
static int         connect_to_supervisor(void);

/* ==============================================================
 * BOUNDED BUFFER
 * ============================================================== */

static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    rc = pthread_mutex_init(&b->mutex, NULL);
    if (rc) return rc;
    rc = pthread_cond_init(&b->not_empty, NULL);
    if (rc) { pthread_mutex_destroy(&b->mutex); return rc; }
    rc = pthread_cond_init(&b->not_full, NULL);
    if (rc) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0) {
        pthread_mutex_unlock(&b->mutex);
        return 1; /* drain complete */
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ==============================================================
 * LOGGING CONSUMER THREAD
 * ============================================================== */
static void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    for (;;) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char log_path[PATH_MAX];
        snprintf(log_path, sizeof(log_path),
                 "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "[logger] open %s: %s\n",
                    log_path, strerror(errno));
            continue;
        }
        size_t written = 0;
        while (written < item.length) {
            ssize_t r = write(fd, item.data + written,
                              item.length - written);
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            written += (size_t)r;
        }
        close(fd);
    }
    return NULL;
}

/* ==============================================================
 * PIPE-READER PRODUCER THREAD
 * ============================================================== */
typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} pipe_reader_arg_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;
    log_item_t item;
    ssize_t n;

    strncpy(item.container_id, pra->container_id,
            sizeof(item.container_id) - 1);
    item.container_id[sizeof(item.container_id) - 1] = '\0';

    while ((n = read(pra->read_fd, item.data, sizeof(item.data))) > 0) {
        item.length = (size_t)n;
        if (bounded_buffer_push(pra->log_buffer, &item) != 0)
            break;
    }
    close(pra->read_fd);
    free(pra);
    return NULL;
}

/* ==============================================================
 * CONTAINER CHILD ENTRY POINT
 * ============================================================== */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* UTS namespace: give the container its own hostname */
    if (sethostname(cfg->id, strlen(cfg->id)) < 0)
        perror("[child] sethostname");

    /* Redirect stdout + stderr through the log pipe */
    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    /* Scheduling priority */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Mount /proc inside rootfs before chroot (path is host-relative here) */
    {
        char proc_path[PATH_MAX];
        snprintf(proc_path, sizeof(proc_path), "%s/proc", cfg->rootfs);
        mkdir(proc_path, 0555);
        mount("proc", proc_path, "proc",
              MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL);
    }

    /* Filesystem isolation */
    if (chroot(cfg->rootfs) < 0) { perror("[child] chroot"); return 1; }
    if (chdir("/")          < 0) { perror("[child] chdir");  return 1; }

    /* Execute */
    char *argv[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv);
    perror("[child] execv");
    return 1;
}

/* ==============================================================
 * KERNEL MONITOR HELPERS
 * ============================================================== */
static int register_with_monitor(int fd, const char *id, pid_t pid,
                                  unsigned long soft, unsigned long hard)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return (ioctl(fd, MONITOR_REGISTER, &req) < 0) ? -1 : 0;
}

static int unregister_from_monitor(int fd, const char *id, pid_t pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = pid;
    strncpy(req.container_id, id, sizeof(req.container_id) - 1);
    return (ioctl(fd, MONITOR_UNREGISTER, &req) < 0) ? -1 : 0;
}

/* ==============================================================
 * SUPERVISOR HELPERS
 * ============================================================== */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0)
            return c;
    return NULL;
}

static const char *termination_reason(const container_record_t *c)
{
    switch (c->state) {
    case CONTAINER_STOPPED: return "stopped";
    case CONTAINER_KILLED:  return "hard_limit_killed";
    case CONTAINER_EXITED:  return "exited";
    default:                return state_to_string(c->state);
    }
}

/*
 * Reap all exited children.
 *
 * Task 4 attribution:
 *   stop_requested=1 + any exit  → CONTAINER_STOPPED
 *   SIGKILL, no flag             → CONTAINER_KILLED (hard-limit path)
 *   other signal, no flag        → CONTAINER_KILLED
 *   normal exit, no flag         → CONTAINER_EXITED
 */
static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c;
        for (c = ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;
            if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                c->state = c->stop_requested ? CONTAINER_STOPPED
                                              : CONTAINER_KILLED;
            } else {
                c->exit_code = WEXITSTATUS(status);
                c->state = c->stop_requested ? CONTAINER_STOPPED
                                             : CONTAINER_EXITED;
            }
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd, c->id, c->host_pid);
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
}

/*
 * Spawn a new container process.
 */
static container_record_t *spawn_container(supervisor_ctx_t *ctx,
                                           const control_request_t *req)
{
    int pipe_fds[2];

    /*
     * pipe2(O_CLOEXEC): both ends close-on-exec by default.
     * We then explicitly clear CLOEXEC on the write end so the
     * child inherits it across clone().  The read end stays
     * CLOEXEC and is never inherited by the child.
     */
    if (pipe2(pipe_fds, O_CLOEXEC) < 0) { perror("pipe2"); return NULL; }
    fcntl(pipe_fds[1], F_SETFD, 0); /* clear CLOEXEC on write end */

    mkdir(LOG_DIR, 0755);

    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(pipe_fds[0]); close(pipe_fds[1]); return NULL; }
    strncpy(cfg->id,      req->container_id, sizeof(cfg->id)      - 1);
    strncpy(cfg->rootfs,  req->rootfs,       sizeof(cfg->rootfs)  - 1);
    strncpy(cfg->command, req->command,      sizeof(cfg->command) - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = pipe_fds[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) { free(cfg); close(pipe_fds[0]); close(pipe_fds[1]); return NULL; }

    int flags  = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t cpid = clone(child_fn, stack + STACK_SIZE, flags, cfg);
    free(stack);
    close(pipe_fds[1]); /* supervisor never writes to container pipe */

    if (cpid < 0) {
        perror("[supervisor] clone");
        free(cfg);
        close(pipe_fds[0]);
        return NULL;
    }
    /* cfg will be freed implicitly when the child's execv() replaces
     * the address space; do not free it here */

    /* Pipe-reader producer thread (detached; exits on pipe EOF) */
    pipe_reader_arg_t *pra = calloc(1, sizeof(*pra));
    if (pra) {
        pra->read_fd    = pipe_fds[0];
        pra->log_buffer = &ctx->log_buffer;
        strncpy(pra->container_id, req->container_id,
                sizeof(pra->container_id) - 1);
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&tid, &attr, pipe_reader_thread, pra) != 0) {
            close(pipe_fds[0]); free(pra);
        }
        pthread_attr_destroy(&attr);
    } else {
        close(pipe_fds[0]);
    }

    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, cpid,
                              req->soft_limit_bytes, req->hard_limit_bytes);

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) return NULL;
    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    rec->host_pid         = cpid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->stop_requested   = 0;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    return rec;
}

/*
 * handle_client – one request/response cycle on a *blocking* client fd.
 *
 * IMPORTANT: the fd passed here must NOT be O_NONBLOCK.
 * recv(MSG_WAITALL) on a non-blocking fd returns EAGAIN immediately
 * if the full struct hasn't arrived yet, which would make every
 * command fail with "bad request size".
 */
static void handle_client(supervisor_ctx_t *ctx, int cfd)
{
    control_request_t  req;
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    /*
     * recv with MSG_WAITALL blocks until all sizeof(req) bytes arrive.
     * This is safe because cfd is blocking (accept4 without SOCK_NONBLOCK).
     */
    ssize_t n = recv(cfd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "bad request (%zd/%zu bytes)", n, sizeof(req));
        send(cfd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *ex = find_container(ctx, req.container_id);
        int busy = (ex && ex->state == CONTAINER_RUNNING);
        pthread_mutex_unlock(&ctx->metadata_lock);
        if (busy) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "container '%s' already running", req.container_id);
            break;
        }
        container_record_t *rec = spawn_container(ctx, &req);
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "spawn failed for '%s'", req.container_id);
        } else {
            resp.status = (int)rec->host_pid;
            snprintf(resp.message, sizeof(resp.message),
                     "started '%s' pid=%d", rec->id, (int)rec->host_pid);
        }
        break;
    }

    case CMD_PS: {
        int off = 0, cap = (int)(sizeof(resp.message) - 1);
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        if (!c)
            off += snprintf(resp.message, (size_t)cap, "(no containers)\n");
        for (; c && off < cap; c = c->next) {
            char extra[64] = "";
            if (c->state == CONTAINER_EXITED || c->state == CONTAINER_STOPPED)
                snprintf(extra, sizeof(extra), " exit=%d", c->exit_code);
            else if (c->state == CONTAINER_KILLED)
                snprintf(extra, sizeof(extra), " signal=%d", c->exit_signal);
            off += snprintf(resp.message + off, (size_t)(cap - off),
                            "%-16s  pid=%-6d  %-18s  soft=%3luMB  hard=%3luMB%s\n",
                            c->id, (int)c->host_pid,
                            termination_reason(c),
                            c->soft_limit_bytes >> 20,
                            c->hard_limit_bytes >> 20,
                            extra);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        break;
    }

    case CMD_LOGS: {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req.container_id);
        int fd = open(path, O_RDONLY);
        if (fd < 0) {
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "no log for '%s'", req.container_id);
        } else {
            ssize_t r = read(fd, resp.message, sizeof(resp.message) - 1);
            resp.message[r > 0 ? r : 0] = '\0';
            resp.status = 0;
            close(fd);
        }
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "unknown container '%s'", req.container_id);
            break;
        }
        if (c->state != CONTAINER_RUNNING) {
            container_state_t st = c->state;
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = -1;
            snprintf(resp.message, sizeof(resp.message),
                     "'%s' not running (state=%s)",
                     req.container_id, state_to_string(st));
            break;
        }
        c->stop_requested = 1;
        pid_t pid = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);
        kill(pid, SIGTERM);
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "SIGTERM -> '%s' pid=%d", req.container_id, (int)pid);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "unknown command %d", req.kind);
        break;
    }

    send(cfd, &resp, sizeof(resp), 0);
}

/* ==============================================================
 * SIGNAL HANDLERS (supervisor)
 * ============================================================== */
static void sig_stop_handler(int sig) { (void)sig; g_stop = 1; }
static void sig_chld_handler(int sig) { (void)sig; }

/* ==============================================================
 * run_supervisor
 * ============================================================== */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    /* Open kernel monitor (optional) */
    ctx.monitor_fd = open("/dev/" DEVICE_NAME, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr,
                "[supervisor] /dev/%s unavailable (%s) "
                "- memory monitoring disabled\n",
                DEVICE_NAME, strerror(errno));

    /* ----------------------------------------------------------
     * Create the UNIX stream socket for the control plane.
     *
     * Critically: only the *listening* fd is set O_NONBLOCK.
     * accept4() is called WITHOUT SOCK_NONBLOCK so accepted client
     * fds are blocking – required for recv(MSG_WAITALL) to work.
     * ---------------------------------------------------------- */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    {
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
        if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("bind"); goto cleanup;
        }
        if (listen(ctx.server_fd, 16) < 0) {
            perror("listen"); goto cleanup;
        }
    }
    /* Set O_NONBLOCK ONLY on the listening fd so select()+accept() works
     * without blocking the event loop.  This flag does NOT propagate to
     * file descriptors returned by accept4() (without SOCK_NONBLOCK). */
    if (fcntl(ctx.server_fd, F_SETFL,
              fcntl(ctx.server_fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        perror("fcntl O_NONBLOCK"); goto cleanup;
    }

    /* Signal handling */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = sig_stop_handler;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
        sa.sa_handler = sig_chld_handler;
        sa.sa_flags   = SA_NOCLDSTOP | SA_RESTART;
        sigaction(SIGCHLD, &sa, NULL);
        signal(SIGPIPE, SIG_IGN);
    }

    /* Start the logger consumer thread */
    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
        perror("pthread_create"); goto cleanup;
    }

    fprintf(stderr, "[supervisor] listening on %s\n", CONTROL_PATH);

    /* ----------------------------------------------------------
     * Event loop:
     *   - select() with 1-second timeout so we reap children
     *     regularly even without incoming connections.
     *   - On EINTR (SIGCHLD, etc.) just continue – do not exit.
     *   - On other select errors, log and continue; only exit
     *     if g_stop is set.
     *   - accept4 without SOCK_NONBLOCK yields a blocking client fd.
     * ---------------------------------------------------------- */
    while (!g_stop) {
        fd_set rfds;
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);

        /* Always reap after waking up (handles SIGCHLD-interrupted select) */
        reap_children(&ctx);

        if (sel < 0) {
            if (errno == EINTR) continue; /* signal interrupted select; loop */
            perror("[supervisor] select");
            continue; /* log and retry rather than exit */
        }

        if (sel == 0)
            continue; /* timeout – just reap and loop */

        if (FD_ISSET(ctx.server_fd, &rfds)) {
            /*
             * accept4 without SOCK_NONBLOCK: the returned fd is blocking.
             * SOCK_CLOEXEC prevents the fd leaking into spawned containers.
             */
            int cfd = accept4(ctx.server_fd, NULL, NULL, SOCK_CLOEXEC);
            if (cfd < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    perror("[supervisor] accept4");
                continue;
            }
            handle_client(&ctx, cfd);
            close(cfd);
        }
    }

    fprintf(stderr, "[supervisor] shutting down\n");

    /* SIGTERM all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    {
        container_record_t *c;
        for (c = ctx.containers; c; c = c->next)
            if (c->state == CONTAINER_RUNNING) {
                c->stop_requested = 1;
                kill(c->host_pid, SIGTERM);
            }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait up to 3 s for containers to exit */
    for (int i = 0; i < 30; i++) {
        reap_children(&ctx);
        int running = 0;
        pthread_mutex_lock(&ctx.metadata_lock);
        container_record_t *c;
        for (c = ctx.containers; c; c = c->next)
            if (c->state == CONTAINER_RUNNING) running++;
        pthread_mutex_unlock(&ctx.metadata_lock);
        if (!running) break;
        usleep(100000);
    }
    reap_children(&ctx);

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    {
        container_record_t *c = ctx.containers;
        while (c) {
            container_record_t *nx = c->next;
            free(c);
            c = nx;
        }
    }

    if (ctx.server_fd  >= 0) close(ctx.server_fd);
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    unlink(CONTROL_PATH);
    pthread_mutex_destroy(&ctx.metadata_lock);
    return 0;
}

/* ==============================================================
 * CLIENT HELPERS
 * ============================================================== */
static int connect_to_supervisor(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr,
                "Cannot connect to supervisor at %s: %s\n"
                "Is `engine supervisor` running?\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_control_request(const control_request_t *req)
{
    int fd = connect_to_supervisor();
    if (fd < 0) return 1;

    control_response_t resp;
    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }
    if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) != (ssize_t)sizeof(resp)) {
        perror("recv"); close(fd); return 1;
    }
    close(fd);

    printf("%s\n", resp.message);
    return (resp.status >= 0) ? 0 : 1;
}

/* ==============================================================
 * CLI PARSING HELPERS
 * ============================================================== */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <rootfs> <command> "
            "[--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs  <id>\n"
            "  %s stop  <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *out)
{
    char *end = NULL;
    errno = 0;
    unsigned long mib = strtoul(value, &end, 10);
    if (errno || end == value || *end) {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value too large for %s\n", flag);
        return -1;
    }
    *out = mib << 20;
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc, char *argv[], int start)
{
    for (int i = start; i < argc; i += 2) {
        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes)) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            char *end = NULL; errno = 0;
            long v = strtol(argv[i+1], &end, 10);
            if (errno || end == argv[i+1] || *end || v < -20 || v > 19) {
                fprintf(stderr, "Invalid --nice: %s\n", argv[i+1]);
                return -1;
            }
            req->nice_value = (int)v;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return -1;
        }
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ==============================================================
 * CMD IMPLEMENTATIONS
 * ============================================================== */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s start <id> <rootfs> <cmd> [...]\n",
                argv[0]);
        return 1;
    }
    control_request_t req; memset(&req, 0, sizeof(req));
    req.kind             = CMD_START;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)       - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)      - 1);
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;
    return send_control_request(&req);
}

/* ------------------------------------------------------------------
 * cmd_run – blocks until the container exits.
 * ------------------------------------------------------------------ */
static volatile sig_atomic_t g_run_interrupted = 0;
static char g_run_id[CONTAINER_ID_LEN];

static void run_sig_forward(int sig) { (void)sig; g_run_interrupted = 1; }

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr, "Usage: %s run <id> <rootfs> <cmd> [...]\n", argv[0]);
        return 1;
    }
    control_request_t req; memset(&req, 0, sizeof(req));
    req.kind             = CMD_RUN;
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs,       argv[3], sizeof(req.rootfs)       - 1);
    strncpy(req.command,      argv[4], sizeof(req.command)      - 1);
    if (parse_optional_flags(&req, argc, argv, 5)) return 1;

    strncpy(g_run_id, req.container_id, sizeof(g_run_id) - 1);

    struct sigaction sa, oi, ot;
    memset(&sa, 0, sizeof(sa)); sigemptyset(&sa.sa_mask);
    sa.sa_handler = run_sig_forward;
    sigaction(SIGINT,  &sa, &oi);
    sigaction(SIGTERM, &sa, &ot);

    /* Send CMD_RUN to supervisor (same as start on supervisor side) */
    int fd = connect_to_supervisor();
    if (fd < 0) return 1;
    control_response_t resp;
    if (send(fd, &req, sizeof(req), 0) != (ssize_t)sizeof(req) ||
        recv(fd, &resp, sizeof(resp), MSG_WAITALL) != (ssize_t)sizeof(resp)) {
        perror("send/recv"); close(fd); return 1;
    }
    close(fd);
    if (resp.status < 0) {
        fprintf(stderr, "Error: %s\n", resp.message);
        return 1;
    }
    fprintf(stderr, "%s\n", resp.message);

    /* Poll for completion */
    int exit_rc = 0;
    for (;;) {
        if (g_run_interrupted) {
            g_run_interrupted = 0;
            control_request_t sr; memset(&sr, 0, sizeof(sr));
            sr.kind = CMD_STOP;
            strncpy(sr.container_id, g_run_id, sizeof(sr.container_id) - 1);
            send_control_request(&sr);
        }

        usleep(200000);

        control_request_t pr; memset(&pr, 0, sizeof(pr));
        pr.kind = CMD_PS;
        int pfd = connect_to_supervisor();
        if (pfd < 0) break;
        control_response_t pr_resp;
        int ok = (send(pfd, &pr, sizeof(pr), 0) == (ssize_t)sizeof(pr) &&
                  recv(pfd, &pr_resp, sizeof(pr_resp), MSG_WAITALL) ==
                      (ssize_t)sizeof(pr_resp));
        close(pfd);
        if (!ok) break;

        /* Scan ps output for our container leaving "running" */
        int done = 0;
        char buf[CONTROL_MSG_LEN];
        strncpy(buf, pr_resp.message, sizeof(buf) - 1);
        buf[sizeof(buf)-1] = '\0';
        char *line = strtok(buf, "\n");
        while (line) {
            size_t idlen = strnlen(g_run_id, CONTAINER_ID_LEN);
            if (strncmp(line, g_run_id, idlen) == 0) {
                if (!strstr(line, "running")) {
                    done = 1;
                    char *ec = strstr(line, "exit=");
                    char *sg = strstr(line, "signal=");
                    if (ec) exit_rc = atoi(ec + 5);
                    if (sg) exit_rc = 128 + atoi(sg + 7);
                }
                break;
            }
            line = strtok(NULL, "\n");
        }
        if (done) break;
    }

    sigaction(SIGINT,  &oi, NULL);
    sigaction(SIGTERM, &ot, NULL);
    return exit_rc;
}

static int cmd_ps(void)
{
    control_request_t req; memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }
    control_request_t req; memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }
    control_request_t req; memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

/* ==============================================================
 * MAIN
 * ============================================================== */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);
    usage(argv[0]);
    return 1;
}
