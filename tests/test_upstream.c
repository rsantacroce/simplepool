/* Tests for the outbound stratum client (src/upstream.c).
 *
 * Two layers:
 *   1. Pure parser tests for mining.notify / mining.subscribe results, using
 *      f2pool- and Ocean-shaped fixtures.
 *   2. A loopback integration test: a throwaway stratum server on 127.0.0.1
 *      drives a real upstream_client through connect -> subscribe -> authorize
 *      -> job/difficulty -> submit -> result.
 */

#define _POSIX_C_SOURCE 200809L
#include "../src/upstream.h"
#include "../src/cjson/cJSON.h"
#include "../src/log.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;
#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

/* ---- parser tests ------------------------------------------------------ */

static void test_parse_notify_f2pool(void) {
    /* f2pool-style mining.notify params. */
    const char *json =
        "[\"6a2f\",\"00000000111111112222222233333333"
        "44444444555555556666666677777777\","
        "\"01000000010000\",\"ffffffff0100\","
        "[\"aa11\",\"bb22\",\"cc33\"],"
        "\"20000000\",\"170355f0\",\"650a1b2c\",true]";
    cJSON *params = cJSON_Parse(json);
    CHECK(params != NULL);

    upstream_job_t j;
    int rc = upstream_parse_notify(params, &j);
    CHECK(rc == 0);
    CHECK(strcmp(j.job_id, "6a2f") == 0);
    CHECK(strncmp(j.prev_hash, "00000000", 8) == 0);
    CHECK(strcmp(j.coinb1, "01000000010000") == 0);
    CHECK(strcmp(j.coinb2, "ffffffff0100") == 0);
    CHECK(j.merkle_count == 3);
    CHECK(strcmp(j.merkle_branch[0], "aa11") == 0);
    CHECK(strcmp(j.merkle_branch[2], "cc33") == 0);
    CHECK(strcmp(j.version, "20000000") == 0);
    CHECK(strcmp(j.nbits, "170355f0") == 0);
    CHECK(strcmp(j.ntime, "650a1b2c") == 0);
    CHECK(j.clean_jobs == 1);

    upstream_job_free_contents(&j);
    cJSON_Delete(params);
}

static void test_parse_notify_ocean_empty_merkle(void) {
    /* Ocean often sends an empty merkle branch (single-tx-aware templates),
     * and clean_jobs=false. */
    const char *json =
        "[\"abc123\",\"deadbeef00000000000000000000000000"
        "0000000000000000000000000000\","
        "\"02000000\",\"00ffffffff\",[],"
        "\"20000000\",\"1700ffff\",\"66000000\",false]";
    cJSON *params = cJSON_Parse(json);
    CHECK(params != NULL);

    upstream_job_t j;
    int rc = upstream_parse_notify(params, &j);
    CHECK(rc == 0);
    CHECK(strcmp(j.job_id, "abc123") == 0);
    CHECK(j.merkle_count == 0);
    CHECK(j.clean_jobs == 0);

    upstream_job_free_contents(&j);
    cJSON_Delete(params);
}

static void test_parse_notify_rejects_short(void) {
    cJSON *params = cJSON_Parse("[\"id\",\"prev\",\"cb1\"]");
    upstream_job_t j;
    CHECK(upstream_parse_notify(params, &j) != 0);
    cJSON_Delete(params);

    /* Not an array at all. */
    cJSON *obj = cJSON_Parse("{\"x\":1}");
    CHECK(upstream_parse_notify(obj, &j) != 0);
    cJSON_Delete(obj);
}

static void test_parse_subscribe_result(void) {
    const char *json =
        "[[[\"mining.set_difficulty\",\"1\"],[\"mining.notify\",\"1\"]],"
        "\"a1b2c3d4\",4]";
    cJSON *r = cJSON_Parse(json);
    CHECK(r != NULL);

    char en1[40] = {0};
    int en2 = 0;
    int rc = upstream_parse_subscribe_result(r, en1, sizeof(en1), &en2);
    CHECK(rc == 0);
    CHECK(strcmp(en1, "a1b2c3d4") == 0);
    CHECK(en2 == 4);
    cJSON_Delete(r);

    /* Malformed: too short. */
    cJSON *bad = cJSON_Parse("[\"x\"]");
    CHECK(upstream_parse_subscribe_result(bad, en1, sizeof(en1), &en2) != 0);
    cJSON_Delete(bad);
}

/* ---- loopback integration test ----------------------------------------- */

typedef struct {
    atomic_int connected;       /* on_state(1) count */
    atomic_int disconnected;    /* on_state(0) count */
    atomic_int jobs;
    atomic_int diffs;
    atomic_int extranonce;
    atomic_int submit_ok;
    atomic_int submit_fail;
    pthread_mutex_t lock;
    char        last_job_id[64];
    double      last_diff;
} obs_t;

static void cb_job(void *ctx, const upstream_job_t *job) {
    obs_t *o = ctx;
    atomic_fetch_add(&o->jobs, 1);
    pthread_mutex_lock(&o->lock);
    snprintf(o->last_job_id, sizeof(o->last_job_id), "%s", job->job_id);
    pthread_mutex_unlock(&o->lock);
}
static void cb_diff(void *ctx, double d) {
    obs_t *o = ctx;
    atomic_fetch_add(&o->diffs, 1);
    o->last_diff = d;
}
static void cb_en(void *ctx, const char *en1, int en2) {
    (void)en1; (void)en2;
    obs_t *o = ctx;
    atomic_fetch_add(&o->extranonce, 1);
}
static void cb_submit(void *ctx, long id, int accepted, const char *err) {
    (void)id; (void)err;
    obs_t *o = ctx;
    if (accepted) atomic_fetch_add(&o->submit_ok, 1);
    else atomic_fetch_add(&o->submit_fail, 1);
}
static void cb_state(void *ctx, int connected) {
    obs_t *o = ctx;
    if (connected) atomic_fetch_add(&o->connected, 1);
    else atomic_fetch_add(&o->disconnected, 1);
}

/* Throwaway stratum server. */
typedef struct {
    int  listen_fd;
    int  port;
    atomic_int stop;
    pthread_t  thr;
} fake_pool_t;

static int send_line(int fd, const char *s) {
    size_t len = strlen(s);
    char *line = malloc(len + 2);
    if (!line) return -1;
    memcpy(line, s, len);
    line[len] = '\n';
    line[len + 1] = '\0';
    ssize_t n = send(fd, line, len + 1, 0);
    free(line);
    return n == (ssize_t)(len + 1) ? 0 : -1;
}

static void *fake_pool_thread(void *arg) {
    fake_pool_t *fp = arg;
    int cfd = accept(fp->listen_fd, NULL, NULL);
    if (cfd < 0) return NULL;

    char buf[8192];
    size_t blen = 0;
    while (!atomic_load(&fp->stop)) {
        ssize_t n = recv(cfd, buf + blen, sizeof(buf) - 1 - blen, 0);
        if (n <= 0) break;
        blen += (size_t)n;
        buf[blen] = '\0';
        for (;;) {
            char *nl = memchr(buf, '\n', blen);
            if (!nl) break;
            *nl = '\0';

            cJSON *root = cJSON_Parse(buf);
            if (root) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
                const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
                long idv = cJSON_IsNumber(id) ? (long)id->valuedouble : 0;
                if (cJSON_IsString(method)) {
                    char line[1024];
                    if (strcmp(method->valuestring, "mining.subscribe") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":[[[\"mining.set_difficulty\","
                            "\"1\"],[\"mining.notify\",\"1\"]],\"deadbeef\",4],"
                            "\"error\":null}", idv);
                        send_line(cfd, line);
                        /* Push difficulty + a job. */
                        send_line(cfd,
                            "{\"id\":null,\"method\":\"mining.set_difficulty\","
                            "\"params\":[512.0]}");
                        send_line(cfd,
                            "{\"id\":null,\"method\":\"mining.notify\",\"params\":"
                            "[\"job42\",\"00000000111111112222222233333333"
                            "44444444555555556666666677777777\",\"0100\",\"ff00\","
                            "[\"aa\",\"bb\"],\"20000000\",\"170355f0\","
                            "\"650a1b2c\",true]}");
                    } else if (strcmp(method->valuestring, "mining.authorize") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":true,\"error\":null}", idv);
                        send_line(cfd, line);
                    } else if (strcmp(method->valuestring, "mining.submit") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":true,\"error\":null}", idv);
                        send_line(cfd, line);
                    }
                }
                cJSON_Delete(root);
            }
            size_t consumed = (size_t)(nl - buf) + 1;
            memmove(buf, buf + consumed, blen - consumed);
            blen -= consumed;
        }
    }
    close(cfd);
    return NULL;
}

static int fake_pool_start(fake_pool_t *fp) {
    memset(fp, 0, sizeof(*fp));
    atomic_init(&fp->stop, 0);
    fp->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fp->listen_fd < 0) return -1;
    int one = 1;
    setsockopt(fp->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001U);  /* 127.0.0.1 (INADDR_LOOPBACK) */
    addr.sin_port = 0;  /* ephemeral */
    if (bind(fp->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) return -1;
    if (listen(fp->listen_fd, 4) != 0) return -1;
    socklen_t alen = sizeof(addr);
    if (getsockname(fp->listen_fd, (struct sockaddr *)&addr, &alen) != 0) return -1;
    fp->port = ntohs(addr.sin_port);
    return pthread_create(&fp->thr, NULL, fake_pool_thread, fp);
}

static void fake_pool_stop(fake_pool_t *fp) {
    atomic_store(&fp->stop, 1);
    shutdown(fp->listen_fd, SHUT_RDWR);
    close(fp->listen_fd);
    pthread_join(fp->thr, NULL);
}

/* Wait until *counter >= want or timeout. Returns 1 if satisfied. */
static int wait_for(atomic_int *counter, int want, int timeout_ms) {
    int waited = 0;
    while (atomic_load(counter) < want && waited < timeout_ms) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 10 * 1000000L };
        nanosleep(&ts, NULL);
        waited += 10;
    }
    return atomic_load(counter) >= want;
}

static void test_loopback_session(void) {
    fake_pool_t fp;
    CHECK(fake_pool_start(&fp) == 0);

    obs_t obs;
    memset(&obs, 0, sizeof(obs));
    pthread_mutex_init(&obs.lock, NULL);

    upstream_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    snprintf(cfg.host, sizeof(cfg.host), "127.0.0.1");
    cfg.port = fp.port;
    snprintf(cfg.user, sizeof(cfg.user), "bcaddr.worker");
    snprintf(cfg.pass, sizeof(cfg.pass), "x");
    cfg.reconnect_min_ms = 100;
    cfg.reconnect_max_ms = 500;

    upstream_callbacks_t cb;
    memset(&cb, 0, sizeof(cb));
    cb.ctx = &obs;
    cb.on_job = cb_job;
    cb.on_set_difficulty = cb_diff;
    cb.on_set_extranonce = cb_en;
    cb.on_submit_result = cb_submit;
    cb.on_state = cb_state;

    upstream_client_t *u = NULL;
    char err[256] = {0};
    int rc = upstream_client_start(&cfg, &cb, &u, err, sizeof(err));
    CHECK(rc == 0);

    CHECK(wait_for(&obs.connected, 1, 3000));    /* authorized */
    CHECK(wait_for(&obs.jobs, 1, 3000));         /* got mining.notify */
    CHECK(wait_for(&obs.diffs, 1, 3000));        /* got set_difficulty */
    CHECK(atomic_load(&obs.extranonce) >= 1);    /* from subscribe result */
    CHECK(obs.last_diff == 512.0);

    pthread_mutex_lock(&obs.lock);
    int have_job = strcmp(obs.last_job_id, "job42") == 0;
    pthread_mutex_unlock(&obs.lock);
    CHECK(have_job);

    /* Submit a share and expect an accept. */
    long sid = upstream_submit(u, "bcaddr.worker", "job42", "00000000",
                               "650a1b2c", "00000000", NULL);
    CHECK(sid > 0);
    CHECK(wait_for(&obs.submit_ok, 1, 3000));

    upstream_client_free(u);
    CHECK(atomic_load(&obs.disconnected) >= 1);  /* on_state(0) at teardown */

    fake_pool_stop(&fp);
    pthread_mutex_destroy(&obs.lock);
}

int main(void) {
    log_init(LOG_LVL_ERROR);   /* keep test output quiet */

    test_parse_notify_f2pool();
    test_parse_notify_ocean_empty_merkle();
    test_parse_notify_rejects_short();
    test_parse_subscribe_result();
    test_loopback_session();

    printf("test_upstream: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
