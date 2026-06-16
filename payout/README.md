# simplepool-payout

Thunder payout worker. Drains `pps_credits.accrued_sats - paid_sats` from
the shared SQLite database by issuing Thunder transactions on a cadence,
then writes back `paid_sats`.

Lives outside the C proxy so the hot path (block-template / share
acceptance) is never blocked on Thunder RPC latency. The C proxy is the
only writer of `accrued_sats`; this worker is the only writer of
`paid_sats`. SQLite WAL + a 5-second busy timeout keep them out of each
other's way.

## Run

```
PAYOUT_DB_PATH=../data/shares.db \
THUNDER_RPC_URL=http://127.0.0.1:6009 \
THUNDER_FROM_ADDRESS=<pool base58 thunder address> \
node index.js
```

Dry run — log what would be paid, skip Thunder RPC and DB writes:

```
PAYOUT_DRY_RUN=1 PAYOUT_DB_PATH=../data/shares.db \
  THUNDER_RPC_URL=http://127.0.0.1:6009 \
  THUNDER_FROM_ADDRESS=any \
  node index.js
```

## Config (environment variables)

| var | required | default | meaning |
| --- | --- | --- | --- |
| `PAYOUT_DB_PATH` | yes | — | path to `data/shares.db` (writable) |
| `THUNDER_RPC_URL` | yes | — | Thunder JSON-RPC endpoint, e.g. `http://127.0.0.1:6009` |
| `THUNDER_FROM_ADDRESS` | yes | — | pool reserve address; must equal `pool_thunder_reserve_address` in `proxy.conf` |
| `THUNDER_RPC_USER` / `THUNDER_RPC_PASS` | no | — | basic-auth if your Thunder node has it (default Thunder build has none) |
| `PAYOUT_INTERVAL_MS` | no | 30000 | how often to scan |
| `PAYOUT_MIN_SATS` | no | 10000 | skip workers below this owed balance |
| `PAYOUT_MAX_PER_TICK` | no | 50 | cap workers paid per scan |
| `PAYOUT_DRY_RUN` | no | — | `1` = log only |
| `PAYOUT_DEBUG` | no | — | `1` = verbose |

## What it does each tick

1. `SELECT … FROM pps_credits JOIN workers WHERE accrued - paid >= min`
2. `thunder.balance()` — bail this tick if the reserve is short
3. For each due worker: `thunder.transfer(addr, owed, fee)`; on success,
   `UPDATE pps_credits SET paid_sats = paid_sats + owed`

Payouts are **not batched** into a single tx — one bad address or RPC
error must not block other miners.

## Known gaps (deliberate)

- **No in-flight ledger.** If Thunder accepts the tx but our `UPDATE`
  fails (process crash between broadcast and commit), we'd double-pay
  the same `owed` next tick. For an MVP that's acceptable; a stricter
  fix is a `payouts_in_flight` table keyed on `(worker_id, txid)`,
  written before broadcast and reconciled on startup.
- **Flat 100-sat fee.** Will need a smarter fee model once Thunder
  fee dynamics are observable. Currently hardcoded in `lib/payout.js`.
- **No confirmation tracking.** Thunder's RPC doesn't expose per-tx
  confirmation counts; we treat a successful broadcast as final.
  That's how the rest of Thunder tooling works today.
