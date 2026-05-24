#define _POSIX_C_SOURCE 200809L
#include "../src/stratum.h"
#include "../src/share.h"
#include "../src/log.h"
#include "../src/cjson/cJSON.h"

#include <arpa/inet.h>
#include <assert.h>
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

/* Observers + state. */
typedef struct {
    int   shares;
    int   rejects;
    int   blocks;
    int   last_is_block;
    char  last_worker[64];
    char  last_reason[128];
} obs_t;

static void on_share(void *ctx, const char *w, const char *addr,
                     uint64_t ts, double d,
                     int is_block, const char *blk) {
    (void)ts; (void)d; (void)blk; (void)addr;
    obs_t *o = ctx;
    o->shares++;
    o->last_is_block = is_block;
    if (is_block) o->blocks++;
    snprintf(o->last_worker, sizeof(o->last_worker), "%s", w ? w : "");
}
static void on_reject(void *ctx, const char *w, uint64_t ts, const char *r) {
    (void)ts; (void)w;
    obs_t *o = ctx;
    o->rejects++;
    snprintf(o->last_reason, sizeof(o->last_reason), "%s", r ? r : "");
}
static void on_block(void *ctx, const char *hex) { (void)ctx; (void)hex; }

/* Helper: parse the first line of an output buffer. Mutates buf (NUL terminator). */
static cJSON *parse_first_line(char *buf) {
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return cJSON_Parse(buf);
}

/* Helper: count newline-delimited messages. */
static int count_lines(const char *buf, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len; ++i) if (buf[i] == '\n') n++;
    return n;
}

/* Standard regtest P2WPKH used in fixtures so the per-connection coinbase
 * renderer can produce a valid scriptPubKey. */
#define TEST_ADDR "bcrt1qw508d6qejxtdg4y5r3zarvary0c5xw7kygt080"

/* Build a tiny job for tests. The coinbase is rendered per-connection at
 * notify/submit time using the miner's address, so the job only carries
 * template-level data. */
static stratum_job_t *make_test_job(const char *job_id,
                                    const uint8_t *network_target_be) {
    uint8_t prev[32] = {0};
    return stratum_job_new(job_id, 1, prev,
                           /*value_sats*/ 5000000000LL,
                           /*wc_hex*/ NULL,
                           /*en1*/ 4, /*en2*/ 4,
                           NULL, 0, 0x1d00ffffu, 0x60000000u,
                           network_target_be, 800000, NULL, 0);
}

static void test_subscribe(void) {
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0 };
    /* Don't start the server — we exercise just the handler. */
    stratum_server_t *s = NULL;
    /* Hack: synthesize a server by calling start with port 0 -> kernel
     * picks one. */
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0); CHECK(s != NULL);
    if (!s) return;

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    rc = stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(out != NULL && olen > 0);
    cJSON *resp = parse_first_line(out);
    CHECK(resp != NULL);
    if (resp) {
        cJSON *result = cJSON_GetObjectItem(resp, "result");
        CHECK(cJSON_IsArray(result));
        CHECK(cJSON_GetArraySize(result) == 3);
        cJSON *subs = cJSON_GetArrayItem(result, 0);
        CHECK(cJSON_IsArray(subs) && cJSON_GetArraySize(subs) == 2);
        cJSON *ex1 = cJSON_GetArrayItem(result, 1);
        CHECK(cJSON_IsString(ex1) && strlen(ex1->valuestring) == 8);
        cJSON *ex2sz = cJSON_GetArrayItem(result, 2);
        CHECK(cJSON_IsNumber(ex2sz) && ex2sz->valueint == 4);
        cJSON_Delete(resp);
    }
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_authorize_triggers_setdiff_notify(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    int rc = stratum_server_start(&cfg, &s);
    CHECK(rc == 0);

    /* Provide a job so notify can be sent. */
    uint8_t net[32]; memset(net, 0xff, 32);
    stratum_server_set_job(s, make_test_job("0001", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    /* Subscribe first to get extranonce1. */
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen);
    free(out); out = NULL; olen = 0;

    rc = stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".w1\",\"x\"]}",
        &out, &olen);
    CHECK(rc == 0);
    /* Expect 3 lines: response(true), set_difficulty, notify. */
    int n = count_lines(out, olen);
    CHECK(n == 3);
    CHECK(strstr(out, "mining.set_difficulty") != NULL);
    CHECK(strstr(out, "mining.notify") != NULL);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".w1") == 0);
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_unknown_job(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    /* No job set. */
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"NOPE\",\"00000000\",\"60000000\",\"00000000\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.rejects == 1);
    CHECK(strstr(obs.last_reason, "stale") != NULL);
    /* response should carry an error array */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

static void test_submit_share_and_dedupe(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1,
                           /* tiny diff -> worker target = max -> any hash passes */
                           .initial_diff = 1e-12,
                           .ctx = &obs, .on_share = on_share,
                           .on_reject = on_reject, .on_block = on_block };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);

    /* Network target = all zeros -> never a block. */
    uint8_t net[32] = {0};
    stratum_server_set_job(s, make_test_job("J1", net));

    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}",
                           &out, &olen); free(out); out=NULL; olen=0;
    stratum_handle_message(s, c,
        "{\"id\":2,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR "\",\"x\"]}",
        &out, &olen); free(out); out=NULL; olen=0;

    int rc = stratum_handle_message(s, c,
        "{\"id\":3,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);
    CHECK(obs.last_is_block == 0);
    CHECK(obs.blocks == 0);
    free(out); out=NULL; olen=0;

    /* Duplicate: same parameters again. */
    rc = stratum_handle_message(s, c,
        "{\"id\":4,\"method\":\"mining.submit\","
        "\"params\":[\"w\",\"J1\",\"deadbeef\",\"60000000\",\"00000001\"]}",
        &out, &olen);
    CHECK(rc == 0);
    CHECK(obs.shares == 1);  /* not incremented */
    CHECK(obs.rejects >= 1);
    CHECK(strstr(obs.last_reason, "duplicate") != NULL);
    free(out);

    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Invalid Bitcoin address as the username must be rejected outright. */
static void test_authorize_rejects_non_address(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs, .on_reject = on_reject };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"alice.w1\",\"x\"]}",
        &out, &olen);
    CHECK(!stratum_conn_authorized_for_test(c));
    CHECK(obs.rejects == 1);
    /* JSON-RPC response carries an error array. */
    CHECK(strstr(out, "\"error\"") != NULL);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* Valid address with a funky label: address is preserved verbatim,
 * worker_name contains the full username (sanitized chars allowed
 * already), payout_address is exactly the address portion. */
static void test_authorize_address_with_label(void) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                           .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    stratum_handle_message(s, c,
        "{\"id\":1,\"method\":\"mining.authorize\","
         "\"params\":[\"" TEST_ADDR ".rig-007\",\"x\"]}",
        &out, &olen);
    CHECK(stratum_conn_authorized_for_test(c));
    CHECK(strcmp(stratum_conn_payout_address_for_test(c), TEST_ADDR) == 0);
    CHECK(strcmp(stratum_conn_worker_name_for_test(c),
                 TEST_ADDR ".rig-007") == 0);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
}

/* ---- hedge routing (Phase 2) ------------------------------------------- */

static void test_route_decide(void) {
    int ov = -1;

    /* fraction 0 -> all solo; fraction 1 -> all pool */
    CHECK(stratum_route_decide(0.0, 0, 0, "addr", &ov) == STRATUM_ROUTE_SOLO);
    CHECK(ov == 0);
    CHECK(stratum_route_decide(1.0, 5, 0, "addr", &ov) == STRATUM_ROUTE_POOL);

    /* per-worker overrides win regardless of fraction */
    CHECK(stratum_route_decide(0.0, 0, 0, "addr.pool", &ov) == STRATUM_ROUTE_POOL);
    CHECK(ov == 1);
    CHECK(stratum_route_decide(1.0, 0, 0, "addr.solo", &ov) == STRATUM_ROUTE_SOLO);
    CHECK(ov == 1);

    /* 20% converges to 2 pool of 10 over a sequential fleet */
    int solo = 0, pool = 0;
    for (int i = 0; i < 10; ++i) {
        if (stratum_route_decide(0.2, solo, pool, "addr", &ov) == STRATUM_ROUTE_POOL)
            pool++;
        else
            solo++;
    }
    CHECK(pool == 2);
    CHECK(solo == 8);

    /* 50% converges to 5/5 */
    solo = pool = 0;
    for (int i = 0; i < 10; ++i) {
        if (stratum_route_decide(0.5, solo, pool, "addr", &ov) == STRATUM_ROUTE_POOL)
            pool++;
        else
            solo++;
    }
    CHECK(pool == 5);
    CHECK(solo == 5);
}

static stratum_route_t authorize_and_route(double frac, int enabled,
                                           const char *worker) {
    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 1, .initial_diff = 1.0,
                          .upstream_enabled = enabled, .pool_fraction = frac,
                          .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    stratum_server_t *s = NULL;
    stratum_server_start(&cfg, &s);
    stratum_conn_t *c = stratum_conn_new_for_test(s);
    char *out = NULL; size_t olen = 0;
    char msg[256];
    snprintf(msg, sizeof(msg),
        "{\"id\":1,\"method\":\"mining.authorize\",\"params\":[\"%s\",\"x\"]}",
        worker);
    stratum_handle_message(s, c, msg, &out, &olen);
    stratum_route_t r = stratum_conn_route_for_test(c);
    free(out);
    stratum_conn_free_for_test(c);
    stratum_server_free(s);
    return r;
}

static void test_route_classification(void) {
    /* .pool override routes to the pool when upstream is enabled */
    CHECK(authorize_and_route(0.0, 1, TEST_ADDR ".pool") == STRATUM_ROUTE_POOL);
    /* .solo override stays solo even at fraction 1.0 */
    CHECK(authorize_and_route(1.0, 1, TEST_ADDR ".solo") == STRATUM_ROUTE_SOLO);
    /* upstream disabled -> always solo, even with a .pool override */
    CHECK(authorize_and_route(0.0, 0, TEST_ADDR ".pool") == STRATUM_ROUTE_SOLO);
    /* fraction 1.0, plain worker -> pool */
    CHECK(authorize_and_route(1.0, 1, TEST_ADDR) == STRATUM_ROUTE_POOL);
}

/* ---- end-to-end bridge (Phase 3) --------------------------------------- *
 * A real miner socket connects to our stratum server; the connection is
 * routed to a pool (pool_fraction=1.0) and bridged to a throwaway upstream
 * pool. We assert the upstream's job is forwarded down and a submit is
 * relayed up and its accept returned to the miner. */

typedef struct {
    int listen_fd;
    int port;
    atomic_int stop;
    pthread_t thr;
} fake_pool_t;

static int fp_send_line(int fd, const char *s) {
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
                char line[1024];
                if (cJSON_IsString(method)) {
                    if (strcmp(method->valuestring, "mining.subscribe") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":[[[\"mining.set_difficulty\","
                            "\"1\"],[\"mining.notify\",\"1\"]],\"deadbeef\",4],"
                            "\"error\":null}", idv);
                        fp_send_line(cfd, line);
                        fp_send_line(cfd,
                            "{\"id\":null,\"method\":\"mining.set_difficulty\","
                            "\"params\":[256.0]}");
                        fp_send_line(cfd,
                            "{\"id\":null,\"method\":\"mining.notify\",\"params\":"
                            "[\"upjob1\",\"00000000111111112222222233333333"
                            "44444444555555556666666677777777\",\"0100\",\"ff00\","
                            "[\"aa\"],\"20000000\",\"170355f0\",\"650a1b2c\",true]}");
                    } else if (strcmp(method->valuestring, "mining.authorize") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":true,\"error\":null}", idv);
                        fp_send_line(cfd, line);
                    } else if (strcmp(method->valuestring, "mining.submit") == 0) {
                        snprintf(line, sizeof(line),
                            "{\"id\":%ld,\"result\":true,\"error\":null}", idv);
                        fp_send_line(cfd, line);
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
    addr.sin_addr.s_addr = htonl(0x7f000001U);
    addr.sin_port = 0;
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

static int tcp_connect_local(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7f000001U);
    addr.sin_port = htons((uint16_t)port);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return fd;
}

/* Read newline-delimited lines from fd until `want` substring is seen or the
 * recv timeout elapses. Returns 1 if found. */
static int read_until(int fd, const char *want, char *acc, size_t acc_cap) {
    size_t used = strlen(acc);
    char buf[4096];
    for (int iter = 0; iter < 50; ++iter) {
        if (strstr(acc, want)) return 1;
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';
        if (used + (size_t)n < acc_cap) {
            memcpy(acc + used, buf, (size_t)n + 1);
            used += (size_t)n;
        }
    }
    return strstr(acc, want) != NULL;
}

static void test_bridge_end_to_end(void) {
    fake_pool_t fp;
    CHECK(fake_pool_start(&fp) == 0);

    obs_t obs = {0};
    stratum_cfg_t cfg = { .bind_port = 0, .max_conns = 4, .initial_diff = 1.0,
                          .upstream_enabled = 1, .pool_fraction = 1.0,
                          .upstream_port = fp.port, .ctx = &obs };
    snprintf(cfg.bind_addr, sizeof(cfg.bind_addr), "127.0.0.1");
    snprintf(cfg.upstream_host, sizeof(cfg.upstream_host), "127.0.0.1");
    snprintf(cfg.upstream_user, sizeof(cfg.upstream_user), "poolacct.w1");
    snprintf(cfg.upstream_pass, sizeof(cfg.upstream_pass), "x");

    stratum_server_t *s = NULL;
    CHECK(stratum_server_start(&cfg, &s) == 0);
    int port = stratum_server_port_for_test(s);
    CHECK(port > 0);

    int fd = tcp_connect_local(port);
    CHECK(fd >= 0);

    fp_send_line(fd, "{\"id\":1,\"method\":\"mining.subscribe\",\"params\":[]}");
    fp_send_line(fd,
        "{\"id\":2,\"method\":\"mining.authorize\",\"params\":[\""
        TEST_ADDR "\",\"x\"]}");

    char acc[16384] = {0};
    /* The upstream job (job_id "upjob1") should be forwarded down to us. */
    CHECK(read_until(fd, "upjob1", acc, sizeof(acc)));
    /* And the upstream extranonce + difficulty should be relayed. */
    CHECK(strstr(acc, "mining.set_extranonce") != NULL);
    CHECK(strstr(acc, "mining.set_difficulty") != NULL);

    /* Submit a share against the upstream job; expect an accept relayed back
     * with our submit id (7). */
    fp_send_line(fd,
        "{\"id\":7,\"method\":\"mining.submit\",\"params\":[\"" TEST_ADDR
        "\",\"upjob1\",\"00000000\",\"650a1b2c\",\"00000000\"]}");
    CHECK(read_until(fd, "\"id\":7", acc, sizeof(acc)));
    CHECK(strstr(acc, "\"result\":true") != NULL);

    close(fd);
    stratum_server_free(s);
    fake_pool_stop(&fp);
}

int main(void) {
    log_init(LOG_LVL_ERROR);   /* quiet the bridge logs during tests */
    test_subscribe();
    test_authorize_triggers_setdiff_notify();
    test_submit_unknown_job();
    test_submit_share_and_dedupe();
    test_authorize_rejects_non_address();
    test_authorize_address_with_label();
    test_route_decide();
    test_route_classification();
    test_bridge_end_to_end();
    printf("test_stratum: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
