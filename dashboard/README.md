# simplepool dashboard

A small read-only Node.js dashboard for the [simplepool](../) solo-mining
stratum server (the one with direct miner payouts + a small operator
fee). It opens a **snapshot** of the proxy's SQLite database in
read-only WAL mode and serves a public web page with hashrate, a worker
leaderboard, recent blocks, a historical "blocks found by the pool"
page, and a per-worker drilldown.

Each worker row in the leaderboard is a `<bitcoin_address>.<rig_label>`
pair (stratum username convention); the `workers.payout_address` column
lets future views roll up multiple rigs per address. The historical
blocks panel at `/blocks` reads `blocks_found` and shows finder,
payout address, on-chain reward, and operator fee for every block the
pool has ever solved.

## Prerequisites

- Node 20+
- The `simplepool` binary writing to a SQLite file (default `../data/shares.db`)
- A periodic snapshot at `../data/shares.snapshot.db` produced via
  `sqlite3 shares.db ".backup shares.snapshot.db"` on a cron — see the
  proxy [README](../README.md#database--dashboard-snapshot).

The dashboard opens the snapshot **read-only**. SQLite's online backup is
atomic, so the snapshot is always a consistent view of the live DB and the
proxy is never blocked by dashboard queries. It is safe to point multiple
dashboard instances at the same snapshot file.

## Install

```
cd simplepool/dashboard
npm install
cp .env.example .env
# edit .env if your DB path or port differs
```

## Run

```
npm start          # production
npm run dev        # auto-restart on file change
```

Defaults: `PORT=8081`, `PROXY_DB_PATH=../data/shares.snapshot.db`.

If the snapshot file doesn't exist yet, the dashboard starts anyway and
displays "no data yet" until the first `.backup` produces it. You can also
point `PROXY_DB_PATH` at the live `shares.db` if you don't want to run a
snapshot cron — SQLite's WAL mode makes that safe too, just less isolated.

## Endpoints

| Path                  | Description                                             |
| --------------------- | ------------------------------------------------------- |
| `/`                   | Overview, leaderboard, last 5 blocks                    |
| `/blocks`             | Full historical "blocks found by the pool", paginated   |
| `/blocks?before=<ts>` | Next page (older than the given UNIX timestamp)         |
| `/worker/:name`       | Per-worker drilldown                                    |
| `/api/overview`       | JSON                                                    |
| `/api/leaderboard`    | JSON                                                    |
| `/api/worker/:name`   | JSON                                                    |
| `/api/blocks`         | JSON paginated, `?limit=N&before=<ts>` (default 50)     |
| `/healthz`            | `{ ok: true, db_ready: bool }`                          |

## Public deployment (nginx)

Reverse-proxy with optional basic auth, gzip, and a tiny cache for `/api/*`:

```nginx
proxy_cache_path /var/cache/nginx/simplepool levels=1:2 keys_zone=simplepool:10m
                 max_size=100m inactive=10m use_temp_path=off;

server {
    listen 80;
    server_name pool.example.com;

    gzip on;
    gzip_types text/plain text/css application/json application/javascript;

    # auth_basic           "simplepool";
    # auth_basic_user_file /etc/nginx/.htpasswd;

    location /api/ {
        proxy_pass         http://127.0.0.1:8081;
        proxy_cache        simplepool;
        proxy_cache_valid  200 5s;
        add_header         X-Cache-Status $upstream_cache_status;
    }

    location / {
        proxy_pass         http://127.0.0.1:8081;
        proxy_set_header   Host $host;
        proxy_set_header   X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header   X-Forwarded-Proto $scheme;
    }
}
```

## How hashrate is estimated

```
H/s ≈ sum(difficulty over window) * 2^32 / window_seconds
```

This is the standard pool estimator. It converges quickly for healthy
workers and is meaningless for workers with very few shares in-window.
