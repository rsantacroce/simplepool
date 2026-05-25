# Upstream Hedge — Status & Resume Notes

Branch: `feature/upstream-pool-jobs`
Last updated: 2026-05-25

Read this first to pick the work back up. Companion docs:
- `docs/upstream-hedge-design.md` — overall architecture + crypto rationale
- `docs/upstream-phase1-plan.md` — the original Phase 1 file-by-file plan

## Goal (one line)

Hedge solo-mining variance by routing a configurable fraction of the fleet to
an upstream pool (f2pool/Ocean) for steady income, while the rest mines the
operator's own solo template.

## Crucial reality (do not forget)

A pool's job commits **the pool's** coinbase into the merkle root the miner
hashes. So:
- A winning hash on pool work is a block **for the pool**; you can't redirect it
  to yourself (changing the coinbase changes the header, invalidating the hash).
- A "near" hash is worthless on-chain — Bitcoin has no partial credit.
- This feature therefore **hedges variance**; it does **not** raise your solo
  block odds. Each miner is in exactly one camp (solo OR pool) at a time.

## What's DONE (Phases 1–3, all merged on this branch, all tests green)

### Phase 1 — outbound stratum client  (`src/upstream.{c,h}`)
- Persistent line-delimited JSON-RPC over raw TCP (NOT HTTP).
- Background thread: connect → `mining.subscribe` → `mining.authorize` →
  receive `mining.notify` / `set_difficulty` / `set_extranonce`; capped
  exponential-backoff reconnect; handles `client.reconnect`.
- `upstream_submit()` is thread-safe (guarded against the reconnecting fd swap).
- Pool-agnostic parsing exposed as pure fns: `upstream_parse_notify`,
  `upstream_parse_subscribe_result`.
- Tests: `tests/test_upstream.c` — parser units (f2pool + Ocean shapes) + a
  loopback integration test against a throwaway local stratum server. 36 asserts.

### Phase 2 — config + route classification
- Config fields (`src/config.{c,h}`): `upstream_enabled`, `upstream_host`,
  `upstream_port`, `upstream_user`, `upstream_pass`, `pool_fraction`
  (validated/clamped). Documented in `proxy.conf.example`.
- `stratum_route_t` {SOLO, POOL}; pure `stratum_route_decide()` — `.solo`/`.pool`
  worker-name suffix forces a route, else a greedy rule converges the live mix
  to `pool_fraction`. Live counts under `route_lock`, released on disconnect.
- Gated on `upstream_enabled` (default off = pure solo).
- Tests in `tests/test_stratum.c` (decide + authorize-path classification).

### Phase 3 — the bridge
- POOL-routed conn on a real socket starts a dedicated `upstream_client_t` at
  authorize (`bridge_try_start` in `src/stratum.c`).
- Downstream ← upstream: `mining.notify` / `set_difficulty` / `set_extranonce`
  forwarded verbatim (`bridge_on_*` callbacks write to the miner socket).
- Downstream → upstream: miner `mining.submit` relayed up under
  `upstream_user`; async accept/reject correlated via the per-conn `pending`
  ring (`bridge_remember_submit` / `bridge_on_submit_result`) and returned to
  the miner with its original id.
- Solo fallback if the bridge can't start (so miners never idle).
- Lifecycle: `upstream_client_free()` (joins thread) runs BEFORE the socket
  closes, so no callback touches a freed conn / closed fd.
- `stratum_cfg` carries `upstream_host/port/user/pass`; wired in `main.c`.
- Test: end-to-end bridge test (real miner socket ↔ our server ↔ throwaway
  upstream pool). `test_stratum` = 59 asserts.

## How to try it

In `proxy.conf`:
```
upstream_enabled = true
upstream_host = btc.f2pool.com      # or an Ocean stratum host
upstream_port = 1314
upstream_user = your_account.worker  # pool account / payout.worker
upstream_pass = x
pool_fraction = 0.2                  # 20% of fleet to the pool
```
Per-rig override: a stratum username ending in `.solo` or `.pool` forces that
camp regardless of `pool_fraction`. With `upstream_enabled=false` behavior is
unchanged (pure solo).

Build/test: `make && make test`.

## Phase 4 — TODO (not started)

1. **Version-rolling (ASICBoost) passthrough.** Today submits are relayed
   faithfully but we never negotiate `mining.configure` upstream. If a
   pool-routed ASIC rolls version and the pool didn't authorize it, those shares
   may be rejected. Either negotiate version-rolling upstream and pass the mask
   through, or disable version-roll on pool-routed conns (suppress our
   downstream `mining.configure` accept when route==POOL).
   - Touch points: `handle_configure` in `stratum.c`; `upstream.c` would need a
     `mining.configure` step before subscribe; forward `version` in
     `upstream_submit` (already plumbed via the submit's 6th param).
2. **Solo-vs-pool stats tagging.** `on_share`/store currently aren't told a
   share is a pool share (only rejects surface via `on_reject`). Add a route
   tag so dashboard accounting stays separable. Signature change to
   `share_observer_fn` (add route/source) ripples to `store.c` + dashboard.
3. **TLS (`stratum+ssl`).** Phase 1 client is plain TCP only. Add optional TLS
   for pools that require it.
4. **Connection multiplexing (optimization).** MVP opens one upstream
   connection per pool-routed miner. For large fleets, multiplex many miners
   over one upstream connection via extranonce1 partitioning (reduce
   extranonce2_size, hand each miner a sub-prefix). Fiddly — only if needed.
5. **Hashrate-weighted split (refinement).** Routing currently counts
   *connections*, not hashrate; rigs differ in size. Weight the decision by
   observed per-miner hashrate for a truer split.
6. **Live mix observability.** Expose solo/pool counts (and pool accept/reject)
   on the dashboard or a stats line.

## Key files / symbols map

| Area | Location |
|------|----------|
| Upstream client | `src/upstream.{c,h}` |
| Bridge callbacks + start | `src/stratum.c` (`bridge_*`, `bridge_try_start`) |
| Bridged submit path | `src/stratum.c` `handle_submit` (early `if (c->up)`) |
| Route decision | `src/stratum.c` `stratum_route_decide`, `conn_assign_route` |
| Route teardown | `src/stratum.c` `conn_thread` done: label, `conn_release_route` |
| Config | `src/config.{c,h}`, `proxy.conf.example` |
| cfg → stratum wiring | `src/main.c` (~line 410+) |
| Tests | `tests/test_upstream.c`, `tests/test_stratum.c` |

## Gotchas learned the hard way

- macOS (Apple clang) hides several POSIX bits that strict `-std=c11` +
  `_POSIX_C_SOURCE=200809L` on glibc requires: include `<sys/time.h>` for
  `struct timeval`, avoid `INADDR_LOOPBACK` (use `htonl(0x7f000001)`),
  `usleep` is gone (use `nanosleep`). Build on the server (Ubuntu/GCC 13) before
  trusting a green macOS build.
- The Linux server repo `/home/forknet/forknet-software/simplepool` is owned by
  `forknet` and may carry **local uncommitted edits** on `main`. Do NOT reset
  it. Verify branches on the server with a throwaway `git worktree` and remove
  it afterward (`git worktree remove ... --force && git worktree prune`).
- Build verified on both macOS and Ubuntu/GCC 13.3.
