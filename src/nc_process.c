#include <signal.h>
#include <nc_conf.h>
#include <nc_process.h>
#include <nc_proxy.h>

static rstatus_t nc_migrate_proxies(struct context *dst, struct context *src);
static rstatus_t nc_setup_listener_for_workers(struct instance *parent_nci, bool reloading);
static rstatus_t nc_spawn_workers(struct array *workers);
static void      nc_worker_process(int worker_id, struct instance *nci);
static rstatus_t nc_shutdown_workers(struct array *workers);

// Global process management states. TODO: set those flags in signal handlers
bool pm_reload = false;
bool pm_respawn = false;
char pm_myrole = ROLE_MASTER;
bool pm_quit = false;

static rstatus_t
nc_clone_instance(struct instance *dst, struct instance *src)
{
    struct context *new_ctx;
    if (dst == NULL || src == NULL) {
        return NC_ERROR;
    }
    nc_memcpy(dst, src, sizeof(struct instance));
    new_ctx = core_ctx_create(dst);
    if (new_ctx == NULL) {
        log_error("failed to create context");
        return NC_ERROR;
    }
    dst->ctx = new_ctx;
    return NC_OK;
}

static rstatus_t
nc_close_other_proxy(void *elem, void *data)
{
    struct instance *nci = (struct instance *)elem, *self = data;
    struct context *ctx = nci->ctx;

    if (nci == self) {
        return NC_OK;
    }

    proxy_deinit(ctx);
    return NC_OK;
}

static rstatus_t
nc_close_other_proxies(struct array *workers, struct instance *self)
{
    return array_each(workers, nc_close_other_proxy, self);
}

// Master process's jobs:
//   1. reload conf
//   2. diff old listening sockets from new, and close outdated sockets
//   3. bind listening sockets for all workers
//   4. spawn workers
//   5. loop for signals
rstatus_t
nc_multi_processes_cycle(struct instance *parent_nci)
{
    rstatus_t status;
    struct context *ctx, *prev_ctx;
    sigset_t set;

    pm_respawn = true; // spawn workers upon start
    status = nc_setup_listener_for_workers(parent_nci, false);
    if (status != NC_OK) {
        log_error("[master] failed to setup listeners");
        return status;
    }

    for (;;) {
        if (pm_reload) {
            pm_reload = false;
            log_debug(LOG_NOTICE, "reloading config");
            ctx = core_ctx_create(parent_nci);
            if (ctx == NULL) {
                log_error("[master] failed to recreate context");
                continue;
            }
            prev_ctx = parent_nci->ctx;
            parent_nci->ctx = ctx;

            status = nc_setup_listener_for_workers(parent_nci, true);
            if (status != NC_OK) {
                // skip reloading
                parent_nci->ctx = prev_ctx;
                continue;
            }
            // TODO: free prev_ctx
            pm_respawn = true; // restart workers
        }

        // FIXME: dealloc master instance memory after reload
        if (pm_respawn) {
            pm_respawn = false;
            status = nc_spawn_workers(&parent_nci->workers);
            if (status != NC_OK) {
                break;
            }
        }

        sigemptyset(&set);
        sigsuspend(&set); // wake when signal arrives. TODO: add timer using setitimer
    }
    return status;
}

static rstatus_t
nc_setup_listener_for_workers(struct instance *parent_nci, bool reloading)
{
    rstatus_t status;
    int i, n = parent_nci->ctx->cf->global.worker_processes;
    int old_workers_n = 0;
    struct instance *worker_nci, *old_worker_nci;
    struct array old_workers;

    if (reloading) {
        old_workers = parent_nci->workers;
        old_workers_n = (int)array_n(&old_workers);
    }

    status = array_init(&parent_nci->workers, (uint32_t)n, sizeof(struct instance));
    if (status != NC_OK) {
        log_error("failed to init parent_nci->workers");
        return status;
    }

    for (i = 0; i < n; i++) {
        worker_nci = array_push(&parent_nci->workers);
        status = nc_clone_instance(worker_nci, parent_nci);
        if (status != NC_OK) {
            return status;
        }
        worker_nci->role = ROLE_WORKER;

        if (reloading && i < old_workers_n) {
            old_worker_nci = (struct instance *)array_get(&old_workers, (uint32_t)i);
            nc_migrate_proxies(worker_nci->ctx, old_worker_nci->ctx);
        }

        status = core_init_listener(worker_nci);
        if (status != NC_OK) {
            return status;
        }
    }
    if (reloading) {
        nc_shutdown_workers(&old_workers);
    }
    return NC_OK;
}

static rstatus_t
nc_spawn_workers(struct array *workers)
{
    int i;
    pid_t pid;
    struct instance *worker_nci;

    ASSERT(array_n(workers) > 0);

    for (i = 0; (uint32_t)i < array_n(workers); ++i) {
        worker_nci = (struct instance *)array_get(workers, (uint32_t)i);
        worker_nci->chan = nc_alloc_channel();
        if (worker_nci->chan == NULL) {
            return NC_ENOMEM;
        }

        switch (pid = fork()) {
        case -1:
            log_error("failed to spawn worker");
            return NC_ERROR;
        case 0:
            pm_myrole = ROLE_WORKER;
            // TODO: setup the communication channel between master and workers
            pid = getpid();
            worker_nci->pid = pid;
            nc_close_other_proxies(workers, worker_nci);
            nc_worker_process(i, worker_nci);
            NOT_REACHED();
        default:
            worker_nci->pid = pid;
            log_debug(LOG_NOTICE, "worker [%d] started", pid);
            break;
        }
    }
    return NC_OK;
}

static rstatus_t
nc_shutdown_workers(struct array *workers)
{
    uint32_t i, nelem;
    void *elem;
    struct chan_msg msg;
    struct instance *worker_nci;

    for (i = 0, nelem = array_n(workers); i < nelem; i++) {
        elem = array_pop(workers);
        worker_nci = (struct instance *)elem;
        // write quit command to worker
        msg.command = NC_CMD_QUIT;
        if (nc_write_channel(worker_nci->chan->fds[0], &msg) <= 0) {
            log_error("failed to send shutdown msg, err %s", strerror(errno));
        }
        nc_dealloc_channel(worker_nci->chan);

        core_ctx_destroy(worker_nci->ctx);
    }
    // TODO: tell old workers to shutdown gracefully
    array_deinit(workers);
    return NC_OK;
}

static void
nc_worker_process(int worker_id, struct instance *nci)
{
    rstatus_t status;
    sigset_t set;

    ASSERT(nci->role == ROLE_WORKER);

    sigemptyset(&set);
    if (sigprocmask(SIG_SETMASK, &set, NULL) == -1) {
        log_error("failed to clear signal mask");
        return;
    }

    status = core_init_instance(nci);
    if (status != NC_OK) {
        log_error("failed to initialize");
        return;
    }

    status = nc_add_channel_event(nci->ctx->evb, nci->chan->fds[1]);
    if (status != NC_OK) {
        log_error("failed to add channel event");
        return;
    }
    // TODO: worker should remove the listening sockets from event base and after lingering connections are exhausted
    // or timeout, quit process.

    for (;!pm_quit;) {
        status = core_loop(nci->ctx);
        if (status != NC_OK) {
            break;
        }
    }
    log_warn("terminated with quit flag: %d", pm_quit);

    exit(0);
}

rstatus_t
nc_single_process_cycle(struct instance *nci)
{
    rstatus_t status;

    status = core_init_listener(nci);
    if (status != NC_OK) {
        return status;
    }
    status = core_init_instance(nci);
    if (status != NC_OK) {
        return status;
    }

    for (;;) {
        status = core_loop(nci->ctx);
        if (status != NC_OK) {
            break;
        }
    }
    return status;
}

void
nc_reload_config(void)
{
    pm_reload = true;
}

// keep the src (old) context's proxies if they exist in the dst (new) context
static rstatus_t
nc_migrate_proxies(struct context *dst, struct context *src)
{
    uint32_t i, nelem, j, nelem2;
    void *elem;
    struct array *src_pools = &src->pool;
    struct array *dst_pools = &dst->pool;
    struct string *src_proxy_name, *src_proxy_addrstr;
    struct string *dst_proxy_name, *dst_proxy_addrstr;
    struct server_pool *src_pool, *dst_pool;

    ASSERT(array_n(src_pools) != 0);
    ASSERT(array_n(dst_pools) != 0);

    for (i = 0, nelem = array_n(src_pools); i < nelem; i++) {
        elem = array_get(src_pools, i);
        src_pool = (struct server_pool *)elem;
        src_proxy_name = &src_pool->name;
        src_proxy_addrstr = &src_pool->addrstr;
        for (j = 0, nelem2 = array_n(dst_pools); j < nelem2; j++) {
            elem = array_get(dst_pools, j);
            dst_pool = (struct server_pool *)elem;
            dst_proxy_name = &dst_pool->name;
            dst_proxy_addrstr = &dst_pool->addrstr;
            if (string_compare(dst_proxy_addrstr, src_proxy_addrstr) == 0) {
                if (string_compare(dst_proxy_name, src_proxy_name) != 0) {
                    log_debug(LOG_NOTICE, "listening socket's name change from [%s] to [%s]", src_proxy_name->data,
                              dst_proxy_name->data);
                }
                if (dst_pool->p_conn != NULL) {
                    log_error("proxy [%s] has been initialized", dst_proxy_name->data); // this should not happen
                    continue;
                }
                log_debug(LOG_NOTICE, "migrate from [%s] [%s]", src_proxy_name->data, src_proxy_addrstr->data);
                dst_pool->p_conn = src_pool->p_conn;
                dst_pool->p_conn->owner = dst_pool;
                src_pool->p_conn = NULL; // p_conn is migrated, so clear the src_pool
            }
        }
    }
    return NC_OK;
}