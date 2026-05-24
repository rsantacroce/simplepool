# Phase 1 Plan — Standalone Upstream Stratum Client

Foundation for the hedge feature: a module that connects *outbound* to a pool
(f2pool, Ocean, …) as if it were a miner, receives jobs, and can submit shares.
Pool-agnostic. No routing/bridging yet — that's Phase 2/3. Phase 1 is standalone
and exercised only by tests.

## Transport decision

Stratum is a **persistent, bidirectional, line-delimited JSON-RPC stream over a
raw TCP socket** — not HTTP. So this module mirrors the socket conventions in
`src/stratum.c` (the server's per-connection thread), **not** the libcurl/HTTP
style in `bitcoind.c`. Reused patterns:

- Newline-framed read buffer: `recv` into a buffer, `memchr(buf,'\n')`, NUL the
  line, `memmove` the remainder (`stratum.c:994–1025`).
- `write_all()` send loop with `EINTR` retry (`stratum.c:967–975`).
- `atomic_int stop`, `pthread_mutex_t write_lock`, thread lifecycle
  (`stratum.c:1071–1165`).
- cJSON parse/build + `cJSON_PrintUnformatted` → `free` (`stratum.c:907–931`,
  `468–476`).
- `LOG_*` macros; `set_err(errbuf, errlen, ...)` for error strings.
- `#define _POSIX_C_SOURCE 200809L` at top of the .c (matches the rest).

## New files

| File | Purpose |
|------|---------|
| `src/upstream.h` | Public types + API |
| `src/upstream.c` | Implementation |
| `tests/test_upstream.c` | Parser unit tests + loopback integration test |
| `tests/test_upstream.mk` | Test build target |

Edited: `Makefile` (add `src/upstream.c` to `SRCS`, `include tests/test_upstream.mk`,
add `build/test_upstream` to the `test` target).

## `src/upstream.h`

```c
#ifndef UPSTREAM_H
#define UPSTREAM_H
#include <stddef.h>

/* One parsed mining.notify job, forwarded ~verbatim downstream later.
 * Hex fields kept as received (stratum word-order); coinbase/branches are
 * heap-allocated because they vary in size across pools. */
typedef struct {
    char   job_id[64];
    char   prev_hash[72];      /* hex */
    char  *coinb1;             /* hex, malloc'd */
    char  *coinb2;             /* hex, malloc'd */
    char **merkle_branch;      /* array of malloc'd hex strings */
    int    merkle_count;
    char   version[16];        /* hex */
    char   nbits[16];          /* hex */
    char   ntime[16];          /* hex */
    int    clean_jobs;
} upstream_job_t;

void upstream_job_free_contents(upstream_job_t *j);

typedef struct {
    char host[256];
    int  port;
    char user[256];            /* pool worker, e.g. "bcaddr.worker" */
    char pass[64];             /* usually "x" */
    int  reconnect_min_ms;     /* backoff floor (e.g. 1000) */
    int  reconnect_max_ms;     /* backoff ceiling (e.g. 30000) */
} upstream_cfg_t;

typedef struct {
    void *ctx;
    void (*on_job)(void *ctx, const upstream_job_t *job);
    void (*on_set_difficulty)(void *ctx, double difficulty);
    void (*on_set_extranonce)(void *ctx, const char *en1_hex, int en2_size);
    void (*on_submit_result)(void *ctx, long id, int accepted, const char *err);
    void (*on_state)(void *ctx, int connected);   /* 1=up after authorize, 0=down */
} upstream_callbacks_t;

typedef struct upstream_client upstream_client_t;   /* opaque */

int  upstream_client_start(const upstream_cfg_t *cfg,
                           const upstream_callbacks_t *cb,
                           upstream_client_t **out,
                           char *errbuf, size_t errlen);

/* Thread-safe. Returns the JSON-RPC id used (>0), or negative on write error. */
long upstream_submit(upstream_client_t *u,
                     const char *worker, const char *job_id,
                     const char *extranonce2, const char *ntime,
                     const char *nonce, const char *version /* nullable */);

void upstream_client_stop(upstream_client_t *u);
void upstream_client_free(upstream_client_t *u);

/* ---- exposed for unit tests (pure, no I/O) ---- */
int upstream_parse_notify(const void *cjson_params, upstream_job_t *out);
int upstream_parse_subscribe_result(const void *cjson_result,
                                    char *en1_hex, size_t en1_cap,
                                    int *en2_size);

#endif
```

## `src/upstream.c` — internals

```c
struct upstream_client {
    upstream_cfg_t       cfg;
    upstream_callbacks_t cb;
    int                  fd;             /* -1 when disconnected */
    pthread_t            thr;
    int                  thr_started;
    atomic_int           stop;
    atomic_long          next_id;        /* JSON-RPC id counter, starts at 1 */
    pthread_mutex_t      write_lock;     /* serializes write_all() */
    long                 subscribe_id;   /* to match the subscribe response */
    long                 authorize_id;
    char                 en1_hex[32];    /* current extranonce1 */
    int                  en2_size;
    double               difficulty;
};
```

### Worker thread (`upstream_thread`)

```
while (!stop):
    fd = tcp_connect(host, port)              # getaddrinfo + connect, TCP_NODELAY
    if fd < 0: sleep(backoff); backoff*=2 (cap); continue
    backoff = reconnect_min_ms
    send mining.subscribe  (id=subscribe_id, params=["simplepool/0.1"])
    # read+parse loop (newline framed, like stratum.c:994):
    for each line:
        root = cJSON_Parse(line)
        if has "method":                      # server notification
            mining.notify         -> upstream_parse_notify -> cb.on_job
            mining.set_difficulty -> cb.on_set_difficulty
            mining.set_extranonce -> update en1/en2 -> cb.on_set_extranonce
            client.reconnect      -> break (force reconnect)
        else:                                  # response to one of our requests
            id == subscribe_id -> parse result (en1, en2_size);
                                  then send mining.authorize (id=authorize_id)
            id == authorize_id -> result==true ? cb.on_state(1) : log+disconnect
            else               -> cb.on_submit_result(id, result==true, err)
        cJSON_Delete(root)
    cb.on_state(0); close(fd); sleep(backoff)
```

### `upstream_submit`

Build `{"id":N,"method":"mining.submit","params":[worker,job_id,en2,ntime,nonce
(,version)]}`, append `\n`, `write_all` under `write_lock`, return `N`. Safe to
call from any thread (e.g. a downstream miner's thread in Phase 3).

### Lifecycle

- `upstream_client_start`: validate cfg, `calloc`, copy cfg/cb, init mutex +
  atomics, `pthread_create`, return 0.
- `upstream_client_stop`: `atomic_store(stop,1)`, `shutdown(fd, SHUT_RDWR)` to
  unblock `recv`, `pthread_join`.
- `upstream_client_free`: stop, destroy mutex, free.

## Pool-agnostic notes (f2pool + Ocean)

- Both speak stratum v1 over plain TCP. Subscribe result shape:
  `[[["mining.set_difficulty",id],["mining.notify",id]], en1_hex, en2_size]`.
  `upstream_parse_subscribe_result` reads `result[1]` (en1) and `result[2]`
  (en2_size); tolerate the subscriptions array being absent/varied.
- Some pools also (re)issue extranonce via `mining.set_extranonce` — handled.
- **Version-rolling / `mining.configure`**: skipped in Phase 1 (no ASICBoost on
  the pool leg). Phase 4 adds it if needed.
- **TLS** (`stratum+ssl`): out of scope for Phase 1; plain TCP only.
- Keep coinbase/merkle buffers heap-allocated — Ocean's templates can be large.

## Testing (`tests/test_upstream.c`)

1. **Parser unit tests** (no sockets): feed canned f2pool and Ocean
   `mining.notify` params and subscribe results into `upstream_parse_notify` /
   `upstream_parse_subscribe_result`; assert every field. Covers pool-shape
   differences cheaply.
2. **Loopback integration test**: start a tiny throwaway stratum server on
   `127.0.0.1:0` in a thread that (a) answers subscribe with a canned result,
   (b) pushes a `mining.notify` + `mining.set_difficulty`, (c) accepts a submit
   and returns `{result:true}`. Point `upstream_client_start` at it and assert
   `on_set_extranonce`, `on_job`, `on_set_difficulty`, `on_state(1)`, and
   `on_submit_result(accepted)` all fire. Mirrors the real-socket style already
   used in `test_stratum.c`.
3. Use the existing `CHECK(...)` macro and pass/fail counters.

`tests/test_upstream.mk`:
```makefile
build/test_upstream: tests/test_upstream.c src/upstream.c src/log.c \
                     src/cjson/cJSON.c
	@mkdir -p build
	$(CC) $(CFLAGS) $(LDFLAGS) -o build/test_upstream $^ -lpthread
```

## Scope / effort

- `upstream.c` ≈ 350–450 LOC; `upstream.h` ≈ 60; tests ≈ 250.
- No changes to `config.c`/`main.c` yet — those land in Phase 2 (config +
  routing) and Phase 3 (bridge). Phase 1 ships a tested, self-contained client.

## Acceptance criteria

- `make` and `make test` pass on the server (Ubuntu/GCC 13) and macOS.
- Parser tests pass for both f2pool- and Ocean-shaped messages.
- Loopback test exercises connect → subscribe → authorize → job → submit →
  result and reconnect-on-drop.
```
