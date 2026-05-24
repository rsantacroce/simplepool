# Upstream Hedge: Solo + Pool Hashrate Split

## Goal

Let the operator hedge solo-mining variance by routing a configurable fraction
of the connected fleet to an upstream pool (f2pool, Ocean, …) for steady PPLNS
income, while the rest keeps mining the operator's own block template (solo
lottery for the full block reward).

## What this is NOT (important)

A pool's job commits *the pool's* coinbase into the merkle root the miner hashes.
Therefore:

- A hash done against an upstream pool's job can **only** find a block for that
  pool. The reward is theirs; they pay us per their payout scheme.
- We cannot "mix" pool hashes into our solo template to raise our own
  block-finding odds. Solo odds are exactly `our_hashrate / network_hashrate`,
  independent of routing.

So this feature does **not** increase the chance we solo-find a block. It trades
some of the fleet's variance for steady third-party income. Each miner is in
exactly one camp at a time (solo OR pool), never both.

## Architecture

```
                         ┌─────────────────────────────────────────┐
   miners ──TCP──▶  stratum_server (existing)                       │
                         │   per-connection route flag:             │
                         │     ROUTE_SOLO  ─▶ existing solo path     │
                         │                    (our bitcoind template)│
                         │     ROUTE_POOL  ─▶ upstream bridge ───────┼──▶ f2pool/Ocean
                         └─────────────────────────────────────────┘    (outbound stratum)
```

### Connection model (MVP): one upstream connection per pool-routed miner

When a downstream miner is classified `ROUTE_POOL`, we open a dedicated outbound
stratum connection to the upstream pool and bridge it 1:1:

- Upstream `extranonce1` / `extranonce2_size` are passed straight through to the
  downstream miner — no extranonce-space partitioning needed.
- Upstream `mining.notify` is forwarded verbatim to the miner.
- Upstream `mining.set_difficulty` is forwarded as the miner's difficulty.
- Downstream `mining.submit` is translated (our pool worker name) and submitted
  upstream; the accept/reject result is relayed back.

Trade-off: N pool-routed miners = N outbound connections. Fine for a small
fleet. A later optimization can multiplex many miners over one upstream
connection via extranonce1 partitioning (reduce extranonce2_size, hand each
miner a sub-prefix) — deferred, it's fiddly and error-prone.

### Routing / classification

- Config `pool_fraction` (0.0–1.0): target share of the fleet sent to the pool.
- MVP assigns at `mining.authorize` time by connection count to track toward the
  target ratio. (Refinement: weight by observed per-miner hashrate, since rigs
  differ — connection-count is only an approximation of a true hashrate split.)
- Per-worker override: a worker-name suffix (`.solo` / `.pool`) forces a camp,
  bypassing the ratio. Useful for pinning a specific rig.
- If `pool_fraction == 0` or upstream is disabled, behavior is identical to
  today (pure solo).

### Failure handling

- If the upstream connection drops or can't be established, a pool-routed miner
  **falls back to solo** rather than idling (keeps the fleet productive).
- Reconnect with capped exponential backoff.

## Config additions (`config.c` / `config.h`)

```
upstream_enabled   bool      # master switch
upstream_host      string
upstream_port      int
upstream_user      string    # our pool account / payout, e.g. "bcaddr.worker"
upstream_pass      string    # usually "x"
pool_fraction      double    # 0.0–1.0 of fleet routed to upstream
```

## New module: `src/upstream.c` / `src/upstream.h`

A standalone outbound stratum v1 client:

- `upstream_connect()` — TCP connect, `mining.subscribe`, `mining.authorize`.
- Parse `mining.set_difficulty`, `mining.notify`, `mining.set_extranonce`,
  and submit responses.
- Callbacks: `on_job`, `on_set_difficulty`, `on_submit_result`.
- `upstream_submit()` — forward a share upstream.
- Reconnect/backoff loop.
- (Later) `mining.configure` for BIP310 version-rolling passthrough.

## Touch points in existing code

- `stratum.c` connection struct: add `route` enum + (for pool) an
  `upstream_conn_t *`.
- `handle_authorize` (`stratum.c:629`): classify route; for pool route, start
  the upstream bridge instead of `send_current_notify`.
- `send_current_notify` (`stratum.c:536`): only solo-routed conns get our job.
- `handle_submit` (`stratum.c:731`): pool-routed conns forward upstream;
  solo-routed conns unchanged.
- Stats/store: tag shares as solo vs pool so payouts/metrics stay separable.

## Protocol notes

- f2pool and Ocean both speak stratum v1 over plain TCP; TLS (`stratum+ssl`) is
  a later add.
- Ocean pushes its own non-standard templates but accepts standard v1 clients.
- Version-rolling (ASICBoost): if downstream miners negotiate `mining.configure`
  version-rolling, we must negotiate the same mask upstream and pass it through.
  MVP can disable version-rolling on pool-routed miners to keep it simple.

## Phased implementation plan

1. **Upstream client** (`upstream.c/.h`): connect to one pool with a test
   account, subscribe/authorize, parse + log jobs, submit a share, handle
   accept/reject. Standalone and independently testable. *(Foundation.)*
2. **Config + routing**: add config fields, the `route` enum, fraction-based
   classification, and per-worker override.
3. **Bridge**: wire pool-routed downstream connections to a dedicated upstream
   connection — forward notify/difficulty/extranonce down, submits up, relay
   results. Handle `clean_jobs`.
4. **Robustness**: reconnect/backoff, solo fallback on upstream failure,
   solo-vs-pool stats tagging, optional version-rolling passthrough.
```
