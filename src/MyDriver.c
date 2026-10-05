/*
 * main.c - CNC driver: QNX resource manager.
 *
 *   read()  /dev/cnc/plant    latest snapshot, answered at once
 *   write() /dev/cnc/methods  one method call, answered when the machine answers
 *
 * io_write does not answer the writer itself: it queues the command together
 * with the writer's rcvid.  The command worker takes commands from the queue
 * in order, runs each on the machine and then replies to that rcvid.  So pool
 * threads never wait on the network, and every writer gets its own answer.
 *
 * Shutdown order (SIGINT / SIGTERM):
 *   1. io_write stops accepting new commands (they get ECANCELED).
 *   2. write_worker finishes the in-flight method (bounded by TIMEOUT_MS),
 *      then drains the queue, replying ECANCELED to every remaining waiter.
 *   3. Both OPC UA sessions close.
 *   4. Process exits.  Any pool thread still mid-reply to a reader is
 *      abandoned by the OS; readers are idempotent and can be retried.
 *
 * Usage: MyDriver [-U uid:gid] url
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cnc_attr;
#define IOFUNC_ATTR_T       struct cnc_attr
#define THREAD_POOL_PARAM_T dispatch_context_t

#include <sys/iofunc.h>
#include <sys/dispatch.h>

#include "opcua_cnc_map.h"
#include "opcua_client.h"
#include "cnc_log.h"

#define ROOT        "DMG MORI CTX 650 CNC Lathe"
#define NS_URI      "http://manufacturing.example.com/cnc"
#define PERIOD_MS   500
#define TIMEOUT_MS  2000
#define GET_PRIO    10          /* reading the plant              */
#define CMD_PRIO    12          /* running commands: above reads  */

typedef enum { PATH_PLANT, PATH_METHODS, PATH_COUNT } path_id_t;

struct cnc_attr {
    iofunc_attr_t attr;         /* must be first */
    path_id_t     id;
};

static const struct { const char *path; mode_t mode; } path_def[PATH_COUNT] = {
    [PATH_PLANT]   = { CNC_PATH_PLANT,   0444 },
    [PATH_METHODS] = { CNC_PATH_METHODS, 0220 },
};

/* ------------------------------------------------------------------ */
/* Command queue                                                      */
/* ------------------------------------------------------------------ */

#define QLEN 16                  /* parallel writes beyond this get EBUSY */

static struct {
    int       rcvid;            /* 0 = writer withdrew; skip, do not reply */
    cnc_cmd_t cmd;
} q[QLEN];

static int             q_head, q_count;     /* ring buffer, protected by q_lock */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static sem_t           q_items;             /* one post per enqueued command   */

/* The command currently being executed by write_worker.  Protected by q_lock.
 *
 * Invariants:
 *   - Only write_worker performs IDLE -> RUNNING and RUNNING -> IDLE.
 *   - io_unblock may only clear rcvid; it never changes state.
 *   - rcvid == 0 while RUNNING means the client withdrew: the method still
 *     runs to completion on the machine, but no reply is due. */
typedef enum { CMD_IDLE, CMD_RUNNING } cmd_state_t;

static struct {
    cmd_state_t state;
    int         rcvid;
} running = { .state = CMD_IDLE, .rcvid = 0 };

static _Atomic int shutting_down;

static pthread_t write_worker_TID;

/* ------------------------------------------------------------------ */
/* Reply helper                                                       */
/* ------------------------------------------------------------------ */

static void reply(int rcvid, int rc)
{
    int r = (rc == EOK)
          ? MsgReply(rcvid, sizeof(cnc_cmd_t), NULL, 0)
          : MsgError(rcvid, rc);
    if (r == -1)
        CNC_LOG("WARN", "reply to rcvid %d failed: %s", rcvid, strerror(errno));
}

/* ------------------------------------------------------------------ */
/* Command worker                                                     */
/* ------------------------------------------------------------------ */

/* Reply ECANCELED to everything still queued.  Called only from write_worker,
 * and only after shutting_down is set (no new writes can arrive). */
static void drain_queue(void)
{
    for (;;) {
        pthread_mutex_lock(&q_lock);
        if (q_count == 0) {
            pthread_mutex_unlock(&q_lock);
            return;
        }
        int rcvid = q[q_head].rcvid;
        q_head = (q_head + 1) % QLEN;
        q_count--;
        pthread_mutex_unlock(&q_lock);
        if (rcvid)
            reply(rcvid, ECANCELED);
    }
}

static void *write_worker(void *unused)
{
    (void)unused;

    /* See the note in main() about PROCMGR_AID_SCHEDULE. */
    struct sched_param sp = { .sched_priority = CMD_PRIO };
    int prc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    if (prc != 0)
        CNC_LOG("WARN", "write_worker: SCHED_FIFO/%d not applied: %s",
                CMD_PRIO, strerror(prc));

    for (;;) {
        sem_wait(&q_items);                          /* wait for a command */

        pthread_mutex_lock(&q_lock);
        if (atomic_load_explicit(&shutting_down, memory_order_acquire)) {
            pthread_mutex_unlock(&q_lock);
            drain_queue();
            return NULL;
        }

        int       rcvid = q[q_head].rcvid;
        cnc_cmd_t cmd   = q[q_head].cmd;
        q_head = (q_head + 1) % QLEN;
        q_count--;

        running.state = CMD_RUNNING;
        running.rcvid = rcvid;
        pthread_mutex_unlock(&q_lock);

        if (rcvid == 0) {
            /* Writer withdrew before we got to it: nothing ran, nobody to answer. */
            pthread_mutex_lock(&q_lock);
            running.state = CMD_IDLE;
            running.rcvid = 0;
            pthread_mutex_unlock(&q_lock);
            continue;
        }

        int rc = opcua_client_call((cnc_method_t)cmd.method, cmd.arg);

        pthread_mutex_lock(&q_lock);
        int reply_to  = running.rcvid;   /* 0 if io_unblock cleared it */
        running.state = CMD_IDLE;
        running.rcvid = 0;
        pthread_mutex_unlock(&q_lock);

        if (reply_to)
            reply(reply_to, rc);

        CNC_LOG("CMD", "method %u arg %d -> %s", (unsigned)cmd.method, (int)cmd.arg,
                rc == EOK ? "OK" : strerror(rc));
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Handlers                                                           */
/* ------------------------------------------------------------------ */

static int io_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb)
{
    cnc_plant_t plant;
    off_t       off;
    int         st;

    if ((st = iofunc_read_verify(ctp, msg, ocb, NULL)) != EOK)
        return st;
    if (ocb->attr->id != PATH_PLANT)
        return ENOSYS;

    switch (msg->i.xtype & _IO_XTYPE_MASK) {
    case _IO_XTYPE_NONE:   off = ocb->offset;                                       break;
    case _IO_XTYPE_OFFSET: off = ((struct _xtype_offset *)(&msg->i + 1))->offset;   break;
    default:               return ENOSYS;
    }

    if (off != 0) {                                  /* one snapshot, then EOF */
        _IO_SET_READ_NBYTES(ctp, 0);
        return EOK;
    }
    if (msg->i.nbytes < sizeof plant)
        return EINVAL;

    opcua_client_get(&plant);
    if ((msg->i.xtype & _IO_XTYPE_MASK) == _IO_XTYPE_NONE)
        ocb->offset += sizeof plant;

    _IO_SET_READ_NBYTES(ctp, sizeof plant);
    if (MsgReply(ctp->rcvid, sizeof plant, &plant, sizeof plant) == -1)
        CNC_LOG("WARN", "io_read: MsgReply failed: %s", strerror(errno));
    return _RESMGR_NOREPLY;
}

static int io_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb)
{
    cnc_cmd_t cmd;
    int       st;

    if ((st = iofunc_write_verify(ctp, msg, ocb, NULL)) != EOK)
        return st;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE || ocb->attr->id != PATH_METHODS)
        return ENOSYS;
    if (msg->i.nbytes != sizeof cmd ||
        resmgr_msgget(ctp, &cmd, sizeof cmd, sizeof msg->i) != (ssize_t)sizeof cmd ||
        cmd.method >= CNC_METHOD_COUNT)
        return EINVAL;

    pthread_mutex_lock(&q_lock);
    /* Take q_lock so this check serialises with the drain in signal_thread():
     * either we enqueue and get drained, or we see the flag and refuse. */
    if (atomic_load_explicit(&shutting_down, memory_order_acquire)) {
        pthread_mutex_unlock(&q_lock);
        return ECANCELED;
    }
    if (q_count == QLEN) {                           /* queue full */
        pthread_mutex_unlock(&q_lock);
        return EBUSY;
    }
    int tail = (q_head + q_count) % QLEN;
    q[tail].rcvid = ctp->rcvid;                      /* remember whom to answer */
    q[tail].cmd   = cmd;
    q_count++;
    pthread_mutex_unlock(&q_lock);

    sem_post(&q_items);                              /* worker takes it from here */
    return _RESMGR_NOREPLY;
}

/* A waiting writer was interrupted (signal or its own timeout). */
static int io_unblock(resmgr_context_t *ctp, io_pulse_t *msg, RESMGR_OCB_T *ocb)
{
    int found = 0;

    pthread_mutex_lock(&q_lock);
    if (running.state == CMD_RUNNING && running.rcvid == ctp->rcvid) {
        /* The method has already started on the machine.  We cannot cancel
         * the in-flight OPC UA call; we only suppress the reply.  The client
         * gets EINTR and must assume the method may have executed (see the
         * EINTR paragraph in opcua_cnc_map.h). */
        running.rcvid = 0;
        found = 1;
    } else {
        for (int i = 0; i < q_count; i++) {
            int k = (q_head + i) % QLEN;
            if (q[k].rcvid == ctp->rcvid) {
                q[k].rcvid = 0;                      /* worker will skip it */
                found = 1;
                break;
            }
        }
    }
    pthread_mutex_unlock(&q_lock);

    if (!found)
        return iofunc_unblock_default(ctp, msg, ocb);

    if (MsgError(ctp->rcvid, EINTR) == -1)
        CNC_LOG("WARN", "io_unblock: MsgError failed: %s", strerror(errno));
    return _RESMGR_NOREPLY;
}

/* ------------------------------------------------------------------ */
/* Shutdown                                                           */
/* ------------------------------------------------------------------ */

static void *signal_thread(void *arg)
{
    int sig;
    sigwait(arg, &sig);
    CNC_LOG("INFO", "signal %d: stopping", sig);

    /* 1. Stop accepting new commands.  Take q_lock so any io_write already
     *    past its shutting_down check lands in the queue and is drained,
     *    while a later io_write sees the flag and returns ECANCELED. */
    pthread_mutex_lock(&q_lock);
    atomic_store_explicit(&shutting_down, 1, memory_order_release);
    pthread_mutex_unlock(&q_lock);
    sem_post(&q_items);         /* wake write_worker if it is idle on the sem */

    /* 2. Finish the in-flight method, then drain the queue.  After the join,
     *    no command writer is still hanging. */
    pthread_join(write_worker_TID, NULL);

    /* 3. Close both OPC UA sessions. */
    opcua_client_stop();

    CNC_LOG("INFO", "stopped");
    exit(EXIT_SUCCESS);
}

/* ------------------------------------------------------------------ */
/* main                                                               */
/* ------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [-U uid:gid] opc.tcp://host:4840/...\n", prog);
    exit(EXIT_FAILURE);
}

int main(int argc, char **argv)
{
    static resmgr_connect_funcs_t connect_funcs;
    static resmgr_io_funcs_t      io_funcs;
    static struct cnc_attr        attrs[PATH_COUNT];
    static sigset_t               sigs;
    resmgr_attr_t                 rattr;
    thread_pool_attr_t            pool;
    dispatch_t                   *dpp;
    pthread_t                     signal_TID;
    int                           c, uid = -1, gid = -1;

    while ((c = getopt(argc, argv, "U:")) != -1)
        if (c != 'U' || sscanf(optarg, "%d:%d", &uid, &gid) != 2)
            usage(argv[0]);
    if (optind != argc - 1)
        usage(argv[0]);

    /* SIGINT/SIGTERM go to the signal thread only */
    sigemptyset(&sigs);
    sigaddset(&sigs, SIGINT);
    sigaddset(&sigs, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &sigs, NULL);

    /* 1. register the paths (the only step that needs root) */
    dpp = dispatch_create();
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max   = 1;
    rattr.msg_max_size = 2048;

    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &connect_funcs, _RESMGR_IO_NFUNCS, &io_funcs);
    io_funcs.read    = io_read;
    io_funcs.write   = io_write;
    io_funcs.unblock = io_unblock;

    for (int i = 0; i < PATH_COUNT; i++) {
        iofunc_attr_init(&attrs[i].attr, S_IFCHR | path_def[i].mode, NULL, NULL);
        attrs[i].id = (path_id_t)i;
        if (resmgr_attach(dpp, &rattr, path_def[i].path, _FTYPE_ANY, 0,
                          &connect_funcs, &io_funcs, &attrs[i]) == -1) {
            perror(path_def[i].path);
            return EXIT_FAILURE;
        }
    }

    /* 2. drop root.
     *
     * This also drops PROCMGR_AID_SCHEDULE, so the pthread_setschedparam()
     * calls inside get_thread() and write_worker() will fail with EPERM and
     * log a WARN.  If you want them actually running at SCHED_FIFO / the
     * requested priorities, either retain the ability here with
     * procmgr_ability(...) before the setuid(), or create those threads
     * before dropping root.  The code is correct either way; only the
     * scheduling class differs. */
    if (uid >= 0 && (setgid(gid) == -1 || setuid(uid) == -1)) {
        perror("dropping root");
        return EXIT_FAILURE;
    }

    /* 3. threads: OPC UA client, command worker, signals */
    opcua_cfg_t cfg = { argv[optind], NS_URI, ROOT, PERIOD_MS, TIMEOUT_MS, GET_PRIO };
    sem_init(&q_items, 0, 0);
    if (opcua_client_start(&cfg) != EOK ||
        /// \callgraph write_worker
        pthread_create(&write_worker_TID, NULL, write_worker, NULL) != EOK ||
        /// \callgraph signal_thread
        pthread_create(&signal_TID, NULL, signal_thread, &sigs) != EOK) {
        fprintf(stderr, "cannot start threads\n");
        return EXIT_FAILURE;
    }
    CNC_LOG("INFO", "serving %s and %s", CNC_PATH_PLANT, CNC_PATH_METHODS);

    /* 4. thread pool: always 2 threads waiting for messages */
    memset(&pool, 0, sizeof pool);
    pool.handle        = dpp;
    pool.context_alloc = dispatch_context_alloc;
    pool.block_func    = dispatch_block;
    pool.handler_func  = dispatch_handler;
    pool.unblock_func  = dispatch_unblock;
    pool.context_free  = dispatch_context_free;
    pool.lo_water      = 2;
    pool.increment     = 1;
    pool.hi_water      = 4;
    pool.maximum       = 8;

    thread_pool_start(thread_pool_create(&pool, POOL_FLAG_EXIT_SELF));   /* does not return */
    return EXIT_FAILURE;
}
