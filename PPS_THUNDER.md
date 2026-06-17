# `pps-thunder` branch — overview & testing

Six commits on top of `main` that turn simplepool from a pure solo-mining
proxy into a dual-mode proxy that can also run as a Thunder-paying PPS
pool. SQLite remains the source of truth; Redis is added as a real-time
event stream alongside it.

```
b0da16f Add BIP300 regtest validation stack
a05a96f Add Thunder payout worker service
571dad4 Support PPS with backend-supplied coinbasetxn (CUSF enforcer)
8783c0b Wire pool_mode=pps end-to-end: Thunder username, drivechain coinbase, PPS accrual
f29ea16 Add Thunder address decoder and drivechain-deposit coinbase builder
b1f3537 Add Redis broadcast for pool events alongside SQLite
```

## What changed

### 1. Redis broadcast (alongside SQLite)
- New module [src/broadcast.c](src/broadcast.c) / [.h](src/broadcast.h).
  Background publisher thread, bounded ring queue, fire-and-forget. SQLite
  remains source of truth; observers never block on Redis I/O.
- Channels published as JSON: `pool:shares`, `pool:rejects`, `pool:blocks`,
  `pool:tip`, `pool:credits`.
- Config: set `redis_url` in `proxy.conf` (empty = module disabled).

### 2. PPS / Thunder pool mode
- New config key `pool_mode = solo | pps` (default `solo`, behaviour
  unchanged). In `pps`:
  - Stratum usernames must be **Thunder addresses** (base58 of the
    20-byte hash, or the `s9_<base58>_<hex6>` deposit-format wrapper).
    Validated in [src/thunder.c](src/thunder.c).
  - Every block's coinbase is a **BIP300 drivechain deposit** into the
    configured `pool_thunder_reserve_address` instead of paying the
    miner on BTC. Operator fee stays in BTC.
  - Each accepted share **credits the worker's `pps_credits.accrued_sats`**
    at `pps_sats_per_diff * difficulty`.
- New coinbase builders ([src/coinbase.c](src/coinbase.c)):
  - `coinbase_build_drivechain` — bare build, emits
    `[OP_DRIVECHAIN(9), OP_RETURN(payload), operator BTC fee, witness commit]`.
  - `coinbase_build_drivechain_from_template` — same shape, but rewrites
    a backend-supplied coinbasetxn (CUSF enforcer path).
- New `pps_credits` table — `worker_id`, `accrued_sats`, `paid_sats`,
  `last_updated`. C proxy only writes `accrued_sats`; payout worker only
  writes `paid_sats`.

### 3. Thunder payout worker
- New service under [payout/](payout/) — Node.js, separate process.
- Polls SQLite every 30s for workers with `accrued - paid >= min_sats`,
  calls Thunder's JSON-RPC `transfer(addr, sats, fee)`, then updates
  `paid_sats` on success. Per-miner payouts are not batched — one bad
  address can't block others.

### 4. BIP300 regtest stack
- Under [scripts/regtest/](scripts/regtest/) — `setup.sh`, `start.sh`,
  `stop.sh`, `status.sh`, `validate.sh`, `inspect-coinbase.sh`.
- Downloads aarch64-darwin prebuilts of `bitcoind-patched`, `electrs`,
  and `bip300301_enforcer` and orchestrates them locally.
- Verified end-to-end: stack comes up clean, enforcer syncs and serves
  `getblocktemplate`. Two prereqs are documented but not yet scripted
  (sidechain #9 activation; choice of stratum miner).

## How to test

### Unit tests (no infra required)

```
make test
```

Runs 7 suites: `share`, `bitcoind`, `stratum`, `store`, `coinbase` (now
12 tests including 3 drivechain ones), `broadcast`, `thunder`. All must
pass. New PPS / drivechain coinbase byte layouts are asserted at the
byte level here.

### Solo mode (unchanged from `main`)

Set `pool_mode = solo` (or omit — that's the default) in `proxy.conf`,
run `./build/simplepool proxy.conf`, point a miner at `:3334` with a
BTC address as the username. Same as before.

### PPS mode against the regtest BIP300 stack

```sh
# one-time
scripts/regtest/setup.sh    # downloads ~50 MB of prebuilt binaries
scripts/regtest/start.sh    # starts bitcoind + electrs + enforcer
scripts/regtest/status.sh   # confirm all up

# bootstrap chain + probe GBT
scripts/regtest/validate.sh

# run the pool against the enforcer (see validate.sh output for the
# exact proxy.conf snippet — uses pool_mode=pps, bitcoind_url=
# http://127.0.0.1:18444, etc.)
./build/simplepool /tmp/regtest-proxy.conf

# in another terminal, connect any stratum miner to 127.0.0.1:13334
# with a Thunder-shaped base58 username

# after a block is mined
scripts/regtest/inspect-coinbase.sh

# teardown
scripts/regtest/stop.sh
```

What still has to be done by hand: BIP300 sidechain #9 activation (a
propose/ack RPC dance against the enforcer at `127.0.0.1:8123`) before
deposits are recognized. Documented in
[scripts/regtest/README.md](scripts/regtest/README.md).

### Redis broadcast

Run a local Redis, set `redis_url = redis://127.0.0.1:6379` in
`proxy.conf`, then in another shell:

```
redis-cli PSUBSCRIBE 'pool:*'
```

Every accepted share / reject / block / tip change / PPS credit will
print as JSON.

### Payout worker (dry run)

```
cd payout && npm install
PAYOUT_DRY_RUN=1 \
PAYOUT_DB_PATH=../data/shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=any \
PAYOUT_INTERVAL_MS=2000 \
node index.js
```

Logs which workers it *would* pay, makes no Thunder RPC calls, doesn't
touch `paid_sats`. Drop `PAYOUT_DRY_RUN=1` once a Thunder node is
reachable to actually pay.

## What's deliberately not done

- **Live Thunder regtest validation.** Stack and pool builders are in
  place; the enforcer-side sidechain activation and miner choice need
  to be plugged in before the deposit can be observed end-to-end.
- **In-flight payout ledger.** If a Thunder tx is broadcast but the
  follow-up `UPDATE pps_credits` crashes, the payout worker would
  double-pay on restart. Acceptable MVP risk; documented in
  [payout/README.md](payout/README.md).
- **Block-withholding statistical audit** (PPS-specific fraud detection).
  Data primitives are in place — left for a follow-up.

## Files of interest

| | |
| --- | --- |
| [src/broadcast.c](src/broadcast.c)/[.h](src/broadcast.h) | Redis publisher |
| [src/thunder.c](src/thunder.c)/[.h](src/thunder.h) | Thunder address decoder |
| [src/coinbase.c](src/coinbase.c) | `coinbase_build_drivechain[_from_template]` |
| [src/stratum.c](src/stratum.c) | Thunder username auth + drivechain coinbase render |
| [src/store.c](src/store.c) | `pps_credits` table + `store_record_credit` |
| [src/main.c](src/main.c) | OP_RETURN payload precompute + PPS accrual wiring |
| [proxy.conf.example](proxy.conf.example) | All new config keys documented inline |
| [payout/](payout/) | Thunder payout service |
| [scripts/regtest/](scripts/regtest/) | BIP300 regtest stack |
