#ifndef SIMPLEPOOL_UPSTREAM_H
#define SIMPLEPOOL_UPSTREAM_H

/* Outbound stratum V1 client.
 *
 * Connects to an upstream pool (f2pool, Ocean, ...) as if it were a miner:
 * subscribe -> authorize -> receive jobs / difficulty -> submit shares.
 * Persistent, line-delimited JSON-RPC over a raw TCP socket (NOT HTTP).
 *
 * A single background thread owns the socket: it connects, performs the
 * subscribe/authorize handshake, then reads notifications and responses,
 * dispatching them through the callback table. Reconnection with capped
 * exponential backoff is automatic. upstream_submit() may be called from
 * any thread; it is serialized with an internal write lock.
 *
 * This module is self-contained (Phase 1): it does no routing or bridging
 * to downstream miners. The callbacks let a future layer forward jobs and
 * relay submit results.
 */

#include <stddef.h>

/* Maximum merkle branch entries we will store for a single job. Pools rarely
 * exceed ~16; this is a generous safety cap. */
#define UPSTREAM_MAX_MERKLE 64

/* One parsed mining.notify job. Hex fields are kept exactly as received
 * (stratum word-order), ready to forward downstream. coinb1/coinb2 and the
 * merkle branch entries are heap-allocated because their sizes vary widely
 * across pools and templates. Always release with upstream_job_free_contents. */
typedef struct {
    char   job_id[64];
    char   prev_hash[72];                       /* hex */
    char  *coinb1;                              /* hex, malloc'd (may be NULL) */
    char  *coinb2;                              /* hex, malloc'd (may be NULL) */
    char  *merkle_branch[UPSTREAM_MAX_MERKLE];  /* malloc'd hex strings */
    int    merkle_count;
    char   version[16];                         /* hex */
    char   nbits[16];                           /* hex */
    char   ntime[16];                           /* hex */
    int    clean_jobs;
} upstream_job_t;

/* Free heap members of a job (not the struct itself). Safe on a zeroed job. */
void upstream_job_free_contents(upstream_job_t *j);

typedef struct {
    char host[256];
    int  port;
    char user[256];            /* pool worker, e.g. "bc1addr.worker" */
    char pass[64];             /* usually "x" */
    int  reconnect_min_ms;     /* backoff floor, e.g. 1000 */
    int  reconnect_max_ms;     /* backoff ceiling, e.g. 30000 */
} upstream_cfg_t;

/* Callback table. All callbacks run on the client's background thread, except
 * none run after upstream_client_stop() returns. Keep them quick and
 * non-blocking; copy anything you need to keep. ctx is passed through. */
typedef struct {
    void *ctx;
    void (*on_job)(void *ctx, const upstream_job_t *job);
    void (*on_set_difficulty)(void *ctx, double difficulty);
    void (*on_set_extranonce)(void *ctx, const char *en1_hex, int en2_size);
    void (*on_submit_result)(void *ctx, long id, int accepted, const char *err);
    void (*on_state)(void *ctx, int connected);  /* 1=up (authorized), 0=down */
} upstream_callbacks_t;

typedef struct upstream_client upstream_client_t;   /* opaque */

/* Start the client. Spawns the background thread and returns immediately; the
 * connection is established asynchronously. cb may be NULL (no callbacks).
 * Returns 0 on success, negative on error (errbuf filled). */
int upstream_client_start(const upstream_cfg_t *cfg,
                          const upstream_callbacks_t *cb,
                          upstream_client_t **out,
                          char *errbuf, size_t errlen);

/* Submit a share upstream. Thread-safe. version may be NULL (no version-roll).
 * Returns the JSON-RPC id used (>0) so the caller can match on_submit_result,
 * or negative on write error (e.g. currently disconnected). */
long upstream_submit(upstream_client_t *u,
                     const char *worker, const char *job_id,
                     const char *extranonce2, const char *ntime,
                     const char *nonce, const char *version);

/* Signal the thread to stop, unblock the socket, and join. Idempotent. */
void upstream_client_stop(upstream_client_t *u);

/* Stop (if needed) and release all resources. Safe on NULL. */
void upstream_client_free(upstream_client_t *u);

/* ---- Pure parsing helpers, exposed for unit tests (no I/O) -------------- *
 * The void* arguments are cJSON nodes (cJSON* / cJSON params array / result),
 * declared as void* so callers of this header need not include cJSON. */

/* Parse a mining.notify params array into *out (caller owns, must free with
 * upstream_job_free_contents). Returns 0 on success, negative on malformed. */
int upstream_parse_notify(const void *cjson_params, upstream_job_t *out);

/* Parse a mining.subscribe result array: extranonce1 hex into en1 (capacity
 * en1_cap) and extranonce2 size into *en2_size. Returns 0 on success. */
int upstream_parse_subscribe_result(const void *cjson_result,
                                    char *en1, size_t en1_cap, int *en2_size);

#endif /* SIMPLEPOOL_UPSTREAM_H */
