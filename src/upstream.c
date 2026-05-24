/* Outbound stratum V1 client. See upstream.h for the contract.
 *
 * One background thread owns the socket lifecycle: connect, subscribe,
 * authorize, then read notifications/responses until the link drops, then
 * reconnect with capped exponential backoff. Reads happen only on this
 * thread; writes (handshake here, plus upstream_submit from any thread) are
 * serialized by write_lock, which also guards the fd against the reconnect
 * that closes and replaces it.
 */

#define _POSIX_C_SOURCE 200809L
#include "upstream.h"
#include "log.h"
#include "cjson/cJSON.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define UPSTREAM_RDBUF (64 * 1024)

struct upstream_client {
    upstream_cfg_t       cfg;
    upstream_callbacks_t cb;
    int                  has_cb;
    int                  fd;            /* -1 when disconnected; guarded by write_lock */
    pthread_t            thr;
    int                  thr_started;
    atomic_int           stop;
    atomic_long          next_id;       /* JSON-RPC id counter */
    pthread_mutex_t      write_lock;    /* serializes writes + fd access */
    long                 subscribe_id;  /* id of the in-flight subscribe */
    long                 authorize_id;  /* id of the in-flight authorize */
    char                 en1_hex[40];   /* current extranonce1 */
    int                  en2_size;
    double               difficulty;
};

/* ---- small helpers ----------------------------------------------------- */

static void set_err(char *errbuf, size_t errlen, const char *fmt, ...) {
    if (!errbuf || errlen == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(errbuf, errlen, fmt, ap);
    va_end(ap);
}

static int write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        off += (size_t)n;
    }
    return 0;
}

/* Sleep up to ms, waking early (in ~100ms steps) if stop is requested. */
static void interruptible_sleep_ms(upstream_client_t *u, int ms) {
    while (ms > 0 && !atomic_load(&u->stop)) {
        int step = ms < 100 ? ms : 100;
        struct timespec ts = { .tv_sec = step / 1000,
                               .tv_nsec = (long)(step % 1000) * 1000000L };
        nanosleep(&ts, NULL);
        ms -= step;
    }
}

/* Duplicate a cJSON string value to the heap, or NULL. */
static char *dup_str_item(const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring) return NULL;
    size_t n = strlen(item->valuestring);
    char *p = malloc(n + 1);
    if (p) memcpy(p, item->valuestring, n + 1);
    return p;
}

/* Copy a cJSON string value into a fixed buffer (truncating). Returns 0 if a
 * string was present, -1 otherwise (dst still NUL-terminated). */
static int copy_str_item(const cJSON *item, char *dst, size_t cap) {
    if (cap == 0) return -1;
    if (!cJSON_IsString(item) || !item->valuestring) { dst[0] = '\0'; return -1; }
    snprintf(dst, cap, "%s", item->valuestring);
    return 0;
}

/* ---- parsing (pure; unit-tested) --------------------------------------- */

void upstream_job_free_contents(upstream_job_t *j) {
    if (!j) return;
    free(j->coinb1);
    free(j->coinb2);
    for (int i = 0; i < j->merkle_count; ++i) free(j->merkle_branch[i]);
    j->coinb1 = j->coinb2 = NULL;
    j->merkle_count = 0;
}

int upstream_parse_notify(const void *cjson_params, upstream_job_t *out) {
    const cJSON *p = cjson_params;
    if (!out || !cJSON_IsArray(p)) return -1;
    if (cJSON_GetArraySize(p) < 9) return -1;

    memset(out, 0, sizeof(*out));

    const cJSON *job_id  = cJSON_GetArrayItem(p, 0);
    const cJSON *prev    = cJSON_GetArrayItem(p, 1);
    const cJSON *cb1     = cJSON_GetArrayItem(p, 2);
    const cJSON *cb2     = cJSON_GetArrayItem(p, 3);
    const cJSON *merkle  = cJSON_GetArrayItem(p, 4);
    const cJSON *version = cJSON_GetArrayItem(p, 5);
    const cJSON *nbits   = cJSON_GetArrayItem(p, 6);
    const cJSON *ntime   = cJSON_GetArrayItem(p, 7);
    const cJSON *clean   = cJSON_GetArrayItem(p, 8);

    if (copy_str_item(job_id,  out->job_id,   sizeof(out->job_id))   != 0 ||
        copy_str_item(prev,    out->prev_hash, sizeof(out->prev_hash)) != 0 ||
        copy_str_item(version, out->version,  sizeof(out->version))  != 0 ||
        copy_str_item(nbits,   out->nbits,    sizeof(out->nbits))    != 0 ||
        copy_str_item(ntime,   out->ntime,    sizeof(out->ntime))    != 0) {
        return -1;
    }

    out->coinb1 = dup_str_item(cb1);
    out->coinb2 = dup_str_item(cb2);
    if (!out->coinb1 || !out->coinb2) { upstream_job_free_contents(out); return -1; }

    if (cJSON_IsArray(merkle)) {
        int n = cJSON_GetArraySize(merkle);
        if (n > UPSTREAM_MAX_MERKLE) { upstream_job_free_contents(out); return -1; }
        for (int i = 0; i < n; ++i) {
            char *h = dup_str_item(cJSON_GetArrayItem(merkle, i));
            if (!h) { upstream_job_free_contents(out); return -1; }
            out->merkle_branch[out->merkle_count++] = h;
        }
    }

    /* clean_jobs: accept bool or number; default false. */
    if (cJSON_IsBool(clean)) out->clean_jobs = cJSON_IsTrue(clean) ? 1 : 0;
    else if (cJSON_IsNumber(clean)) out->clean_jobs = clean->valuedouble != 0.0;

    return 0;
}

int upstream_parse_subscribe_result(const void *cjson_result,
                                    char *en1, size_t en1_cap, int *en2_size) {
    const cJSON *r = cjson_result;
    if (!cJSON_IsArray(r) || cJSON_GetArraySize(r) < 3) return -1;
    /* result = [ subscriptions, extranonce1_hex, extranonce2_size ] */
    const cJSON *e1 = cJSON_GetArrayItem(r, 1);
    const cJSON *e2 = cJSON_GetArrayItem(r, 2);
    if (copy_str_item(e1, en1, en1_cap) != 0) return -1;
    if (!cJSON_IsNumber(e2)) return -1;
    if (en2_size) *en2_size = (int)e2->valuedouble;
    return 0;
}

/* Extract a human-readable message from a JSON-RPC error node, which is
 * typically [code, "message", traceback] but may be a bare string or null. */
static const char *error_message(const cJSON *err) {
    if (!err || cJSON_IsNull(err)) return NULL;
    if (cJSON_IsString(err)) return err->valuestring;
    if (cJSON_IsArray(err)) {
        const cJSON *msg = cJSON_GetArrayItem(err, 1);
        if (cJSON_IsString(msg)) return msg->valuestring;
    }
    return "error";
}

/* ---- request sending --------------------------------------------------- */

/* Serialize {"id":id,"method":method,"params":params} + '\n' and send it.
 * Takes ownership of params (deletes it). Returns 0 on success, -1 on error. */
static int us_send(upstream_client_t *u, long id, const char *method,
                   cJSON *params) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) { cJSON_Delete(params); return -1; }
    cJSON_AddItemToObject(obj, "id", cJSON_CreateNumber((double)id));
    cJSON_AddItemToObject(obj, "method", cJSON_CreateString(method));
    cJSON_AddItemToObject(obj, "params", params ? params : cJSON_CreateArray());

    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!s) return -1;

    size_t len = strlen(s);
    char *line = malloc(len + 2);
    if (!line) { free(s); return -1; }
    memcpy(line, s, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    free(s);

    int rc = -1;
    pthread_mutex_lock(&u->write_lock);
    if (u->fd >= 0) rc = write_all(u->fd, line, len + 1);
    pthread_mutex_unlock(&u->write_lock);
    free(line);
    return rc;
}

long upstream_submit(upstream_client_t *u,
                     const char *worker, const char *job_id,
                     const char *extranonce2, const char *ntime,
                     const char *nonce, const char *version) {
    if (!u || !worker || !job_id || !extranonce2 || !ntime || !nonce) return -1;
    cJSON *p = cJSON_CreateArray();
    if (!p) return -1;
    cJSON_AddItemToArray(p, cJSON_CreateString(worker));
    cJSON_AddItemToArray(p, cJSON_CreateString(job_id));
    cJSON_AddItemToArray(p, cJSON_CreateString(extranonce2));
    cJSON_AddItemToArray(p, cJSON_CreateString(ntime));
    cJSON_AddItemToArray(p, cJSON_CreateString(nonce));
    if (version) cJSON_AddItemToArray(p, cJSON_CreateString(version));

    long id = atomic_fetch_add(&u->next_id, 1);
    if (us_send(u, id, "mining.submit", p) != 0) return -1;
    return id;
}

/* ---- connection -------------------------------------------------------- */

static int tcp_connect(const char *host, int port, char *errbuf, size_t errlen) {
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, portstr, &hints, &res);
    if (gai != 0) {
        set_err(errbuf, errlen, "getaddrinfo %s:%d: %s", host, port, gai_strerror(gai));
        return -1;
    }

    int fd = -1;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);

    if (fd < 0) {
        set_err(errbuf, errlen, "connect %s:%d: %s", host, port, strerror(errno));
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    return fd;
}

/* Handle one decoded message line. Returns 0 normally, -1 to force reconnect. */
static int handle_line(upstream_client_t *u, char *line) {
    cJSON *root = cJSON_Parse(line);
    if (!root) {
        LOG_WARN("upstream: unparseable line from %s", u->cfg.host);
        return 0;
    }

    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    int rc = 0;

    if (cJSON_IsString(method)) {
        /* Server-initiated notification. */
        const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (strcmp(method->valuestring, "mining.notify") == 0) {
            upstream_job_t job;
            if (upstream_parse_notify(params, &job) == 0) {
                if (u->has_cb && u->cb.on_job) u->cb.on_job(u->cb.ctx, &job);
                upstream_job_free_contents(&job);
            } else {
                LOG_WARN("upstream: malformed mining.notify from %s", u->cfg.host);
            }
        } else if (strcmp(method->valuestring, "mining.set_difficulty") == 0) {
            const cJSON *d = cJSON_GetArrayItem(params, 0);
            if (cJSON_IsNumber(d)) {
                u->difficulty = d->valuedouble;
                if (u->has_cb && u->cb.on_set_difficulty)
                    u->cb.on_set_difficulty(u->cb.ctx, u->difficulty);
            }
        } else if (strcmp(method->valuestring, "mining.set_extranonce") == 0) {
            const cJSON *e1 = cJSON_GetArrayItem(params, 0);
            const cJSON *e2 = cJSON_GetArrayItem(params, 1);
            if (copy_str_item(e1, u->en1_hex, sizeof(u->en1_hex)) == 0 &&
                cJSON_IsNumber(e2)) {
                u->en2_size = (int)e2->valuedouble;
                if (u->has_cb && u->cb.on_set_extranonce)
                    u->cb.on_set_extranonce(u->cb.ctx, u->en1_hex, u->en2_size);
            }
        } else if (strcmp(method->valuestring, "client.reconnect") == 0) {
            LOG_INFO("upstream: %s requested reconnect", u->cfg.host);
            rc = -1;
        }
        /* Other notifications (e.g. mining.set_version_mask) ignored for now. */
    } else {
        /* Response to one of our requests. */
        const cJSON *id_it  = cJSON_GetObjectItemCaseSensitive(root, "id");
        const cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
        const cJSON *error  = cJSON_GetObjectItemCaseSensitive(root, "error");
        long id = cJSON_IsNumber(id_it) ? (long)id_it->valuedouble : -1;

        if (id == u->subscribe_id) {
            if (upstream_parse_subscribe_result(result, u->en1_hex,
                                                sizeof(u->en1_hex),
                                                &u->en2_size) == 0) {
                LOG_INFO("upstream: %s subscribed (en1=%s en2_size=%d)",
                         u->cfg.host, u->en1_hex, u->en2_size);
                if (u->has_cb && u->cb.on_set_extranonce)
                    u->cb.on_set_extranonce(u->cb.ctx, u->en1_hex, u->en2_size);
                /* Now authorize. */
                u->authorize_id = atomic_fetch_add(&u->next_id, 1);
                cJSON *p = cJSON_CreateArray();
                cJSON_AddItemToArray(p, cJSON_CreateString(u->cfg.user));
                cJSON_AddItemToArray(p, cJSON_CreateString(u->cfg.pass));
                if (us_send(u, u->authorize_id, "mining.authorize", p) != 0)
                    rc = -1;
            } else {
                LOG_ERROR("upstream: %s bad subscribe result", u->cfg.host);
                rc = -1;
            }
        } else if (id == u->authorize_id) {
            if (cJSON_IsTrue(result)) {
                LOG_INFO("upstream: %s authorized as %s", u->cfg.host, u->cfg.user);
                if (u->has_cb && u->cb.on_state) u->cb.on_state(u->cb.ctx, 1);
            } else {
                const char *msg = error_message(error);
                LOG_ERROR("upstream: %s authorize rejected: %s",
                          u->cfg.host, msg ? msg : "denied");
                rc = -1;
            }
        } else {
            /* Treat as a submit response. */
            int accepted = cJSON_IsTrue(result);
            const char *msg = error_message(error);
            if (u->has_cb && u->cb.on_submit_result)
                u->cb.on_submit_result(u->cb.ctx, id, accepted, msg);
        }
    }

    cJSON_Delete(root);
    return rc;
}

/* Read loop for a connected socket. Returns when the link drops or stop set. */
static void session_loop(upstream_client_t *u) {
    char *buf = malloc(UPSTREAM_RDBUF + 1);
    if (!buf) return;
    size_t blen = 0;

    while (!atomic_load(&u->stop)) {
        ssize_t n = recv(u->fd, buf + blen, UPSTREAM_RDBUF - blen, 0);
        if (n <= 0) break;
        blen += (size_t)n;
        buf[blen] = '\0';
        for (;;) {
            char *nl = memchr(buf, '\n', blen);
            if (!nl) {
                if (blen >= UPSTREAM_RDBUF) {     /* oversize line: drop link */
                    LOG_WARN("upstream: %s oversize line, reconnecting", u->cfg.host);
                    blen = 0;
                    goto out;
                }
                break;
            }
            *nl = '\0';
            int rc = handle_line(u, buf);
            size_t consumed = (size_t)(nl - buf) + 1;
            memmove(buf, buf + consumed, blen - consumed);
            blen -= consumed;
            if (rc < 0) goto out;
        }
    }
out:
    free(buf);
}

static void *upstream_thread(void *arg) {
    upstream_client_t *u = arg;
    int backoff = u->cfg.reconnect_min_ms > 0 ? u->cfg.reconnect_min_ms : 1000;
    int backoff_max = u->cfg.reconnect_max_ms > 0 ? u->cfg.reconnect_max_ms : 30000;

    while (!atomic_load(&u->stop)) {
        char err[256] = {0};
        int fd = tcp_connect(u->cfg.host, u->cfg.port, err, sizeof(err));
        if (fd < 0) {
            LOG_WARN("upstream: %s", err);
            interruptible_sleep_ms(u, backoff);
            backoff = backoff * 2 < backoff_max ? backoff * 2 : backoff_max;
            continue;
        }

        pthread_mutex_lock(&u->write_lock);
        u->fd = fd;
        pthread_mutex_unlock(&u->write_lock);
        backoff = u->cfg.reconnect_min_ms > 0 ? u->cfg.reconnect_min_ms : 1000;

        /* Kick off the handshake: subscribe (authorize follows its response). */
        u->subscribe_id = atomic_fetch_add(&u->next_id, 1);
        cJSON *p = cJSON_CreateArray();
        cJSON_AddItemToArray(p, cJSON_CreateString("simplepool/0.1"));
        if (us_send(u, u->subscribe_id, "mining.subscribe", p) == 0) {
            session_loop(u);
        }

        /* Link down. */
        pthread_mutex_lock(&u->write_lock);
        if (u->fd >= 0) { close(u->fd); u->fd = -1; }
        pthread_mutex_unlock(&u->write_lock);
        if (u->has_cb && u->cb.on_state) u->cb.on_state(u->cb.ctx, 0);

        if (!atomic_load(&u->stop)) {
            LOG_INFO("upstream: %s disconnected, retrying", u->cfg.host);
            interruptible_sleep_ms(u, backoff);
            backoff = backoff * 2 < backoff_max ? backoff * 2 : backoff_max;
        }
    }
    return NULL;
}

/* ---- lifecycle --------------------------------------------------------- */

int upstream_client_start(const upstream_cfg_t *cfg,
                          const upstream_callbacks_t *cb,
                          upstream_client_t **out,
                          char *errbuf, size_t errlen) {
    if (!cfg || !out) { set_err(errbuf, errlen, "null arg"); return -1; }
    if (cfg->host[0] == '\0' || cfg->port <= 0) {
        set_err(errbuf, errlen, "upstream host/port not set");
        return -1;
    }

    upstream_client_t *u = calloc(1, sizeof(*u));
    if (!u) { set_err(errbuf, errlen, "oom"); return -1; }
    u->cfg = *cfg;
    if (cb) { u->cb = *cb; u->has_cb = 1; }
    u->fd = -1;
    atomic_init(&u->stop, 0);
    atomic_init(&u->next_id, 1);
    if (pthread_mutex_init(&u->write_lock, NULL) != 0) {
        set_err(errbuf, errlen, "mutex init failed");
        free(u);
        return -1;
    }

    if (pthread_create(&u->thr, NULL, upstream_thread, u) != 0) {
        set_err(errbuf, errlen, "pthread_create failed");
        pthread_mutex_destroy(&u->write_lock);
        free(u);
        return -1;
    }
    u->thr_started = 1;
    *out = u;
    return 0;
}

void upstream_client_stop(upstream_client_t *u) {
    if (!u) return;
    if (atomic_exchange(&u->stop, 1) != 0) {
        /* Already stopping/stopped; still ensure the thread is joined below. */
    }
    pthread_mutex_lock(&u->write_lock);
    if (u->fd >= 0) shutdown(u->fd, SHUT_RDWR);
    pthread_mutex_unlock(&u->write_lock);
    if (u->thr_started) {
        pthread_join(u->thr, NULL);
        u->thr_started = 0;
    }
    pthread_mutex_lock(&u->write_lock);
    if (u->fd >= 0) { close(u->fd); u->fd = -1; }
    pthread_mutex_unlock(&u->write_lock);
}

void upstream_client_free(upstream_client_t *u) {
    if (!u) return;
    upstream_client_stop(u);
    pthread_mutex_destroy(&u->write_lock);
    free(u);
}
