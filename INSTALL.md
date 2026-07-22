# simplepool — Linux installation guide

Getting a working simplepool deployment on a Linux host, from `git clone`
to accepting real shares and paying miners in Thunder.

Two audiences:

- **Pool operators** — read Part 0 (overview), pick one of Parts 1–3
  (deployment path), then Parts 4–8 (infra + config + verify).
- **Developers** — read Part 0 (overview), skip to Part 9 (local dev +
  regtest stack).

Three deployment paths for operators — **pick one**:

| path | best for | time to first share | reversibility |
|---|---|---|---|
| **A. Docker** (Part 1) | evaluation, portable deploys, avoiding host-level pkg installs | ~15 min | trivial (`docker compose down`) |
| **B. Automated Ubuntu script** (Part 2) | fresh Ubuntu 24.04 box you own | ~10 min | moderate (apt packages installed system-wide) |
| **C. Manual, step-by-step** (Part 3) | other distros, hardened hosts, or when you want to know what every step does | ~30 min | full control |

**All three paths share** the drivechain infrastructure setup
(Part 4), `proxy.conf` (Part 5), dashboard admin config (Part 6),
payout worker (Part 7), and verification (Part 8). You'll walk through
those once regardless of path.

---

## Part 0 — overview

### The three pool modes

Selected via `pool_mode` in `proxy.conf`. This is the single biggest
architectural choice — everything else follows.

- **`pool_mode = solo`** (default) — every accepted block's coinbase
  pays the miner who found it directly. Simplest. No drivechain, no
  Thunder, no PPS accrual. Miner username is a **BTC address**.
- **`pool_mode = pps`** — every coinbase is a BIP300 drivechain deposit
  into the pool's Thunder reserve address. **Empirically does not
  credit Thunder** on the LayerTwo-Labs enforcer (see
  [PPS_THUNDER.md](PPS_THUNDER.md)) — kept only as a byte-level shape
  validator. Do not use in production.
- **`pool_mode = pps-classic`** — traditional coinbase paying a pool
  BTC address; operator batches accumulated BTC into Thunder from the
  admin dashboard's Deposits tab. **This is the mode you actually want
  for a Thunder-paying PPS pool.** Miner username is a **Thunder
  address**. See [CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md).

### Full stack

```
        miner ASIC
           │  stratum+tcp
           ▼
      ┌──────────────────────┐
      │ simplepool (C11)     │   ── SQLite ──┬── dashboard (Node)    :8081
      │   :3334              │               │     - public /
      │                      │               │     - /admin/* (Basic auth)
      └──────┬───────────────┘               │
             │ GBT / submitblock             └── payout worker (Node)
             │                                      - Thunder JSON-RPC :6009
             │                                      - admin HTTP     :9080
             ▼
      ┌──────────────────────┐   gRPC :50051   ┌────────────────┐
      │ bip300301_enforcer   │◀────────────────▶│ Thunder daemon │
      │   :8122 GBT          │                  │   :6009 RPC    │
      │   :50051 gRPC        │                  └────────────────┘
      │   :8123 JSON-RPC     │
      └──────┬───────────────┘
             │ JSON-RPC :8332, ZMQ :29000, REST
             ▼
      ┌──────────────────────┐   Electrum   ┌──────────┐
      │ bitcoind-patched     │◀─────────────│ electrs  │
      │   (BIP300-aware)     │              │   :50001 │
      └──────────────────────┘              └──────────┘
```

For **solo mode**, everything below the enforcer is optional — point
simplepool at a stock bitcoind. For **pps-classic**, Thunder is only
strictly required at payout time (the coinbase itself just pays a
BTC address). For **pps**, Thunder is nominally required but the
deposit flow doesn't work end-to-end anyway.

### Host prerequisites (all paths)

- A Linux host with root or `sudo`. Ubuntu 24.04 / Debian 12 tested;
  other distros work with minor adjustments (RHEL/Fedora notes in
  Part 3).
- **RAM:** 4 GB minimum, 8 GB comfortable. bitcoind + enforcer are the
  hungriest.
- **Disk:** 50 GB for signet or forknet; **500 GB+** for mainnet
  (bitcoind's txindex).
- **Network:** inbound port 3334 (stratum) open, plus 80/443 if you're
  fronting the dashboard with nginx + Let's Encrypt.
- **Sync time:** ~15 min for signet, hours for forknet, days for
  mainnet from scratch.

---

## Part 1 — Docker deploy (Path A)

Fastest way to go from clone to running services. The drivechain
infra (bitcoind, enforcer, Thunder, electrs) is **not** containerized
by this compose stack — they stay bare-metal on the same host (or
elsewhere). Only the three simplepool services run in containers.

### 1.1 Install Docker + Compose

```sh
# Ubuntu 24.04 / Debian 12
sudo apt update
sudo apt install -y ca-certificates curl
sudo install -m 0755 -d /etc/apt/keyrings
sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
sudo chmod a+r /etc/apt/keyrings/docker.asc
echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc] \
    https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
    | sudo tee /etc/apt/sources.list.d/docker.list >/dev/null
sudo apt update
sudo apt install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin

# Verify
docker version
docker compose version
```

### 1.2 Clone + configure

```sh
git clone https://github.com/rsantacroce/simplepool.git
cd simplepool

# Copy the two config templates
cp deploy/docker/.env.example deploy/docker/.env
cp proxy.conf.example         deploy/docker/proxy.conf

# Edit both
$EDITOR deploy/docker/.env
$EDITOR deploy/docker/proxy.conf
```

**Critical for `proxy.conf`** — inside the container the DB path must
be `/data/shares.db`:

```conf
db_path = /data/shares.db
```

Everything else in `proxy.conf` is per-mode config (see Part 5).

**Critical for `.env`** — set at minimum:

```
ADMIN_USER=admin
ADMIN_PASSWORD=<from `openssl rand -base64 21 | tr -d '=+/'`>
POOL_THUNDER_RESERVE_ADDRESS=<must match proxy.conf's pool_thunder_reserve_address>
THUNDER_FROM_ADDRESS=<same as POOL_THUNDER_RESERVE_ADDRESS>
PUBLIC_STRATUM_URL=stratum+tcp://your-pool-host.example.com:3334
```

The Thunder RPC + enforcer gRPC URLs default to `host.docker.internal`
which resolves to the host gateway on Linux (via `extra_hosts:
"host.docker.internal:host-gateway"` in the compose file — already
set for you). If your Thunder / enforcer live elsewhere, override
`THUNDER_RPC_URL` and `ENFORCER_GRPC_ADDR` in `.env`.

### 1.3 Bring up the stack

```sh
cd deploy/docker
docker compose up -d --build
```

First build takes 3–5 min (Node native binding compile). Subsequent
rebuilds are seconds via layer cache.

Watch logs:

```sh
docker compose logs -f simplepool
docker compose logs -f dashboard
docker compose logs -f payout
```

### 1.4 What's running

- `simplepool` — host networking (miners hit `:3334` directly)
- `simplepool-dashboard` on `${DASHBOARD_PORT:-8081}` (bridge network)
- `simplepool-payout` — bridge network; publishes admin HTTP on
  `127.0.0.1:9080` (loopback-only)

Shared bind mount: `../../data/` (host) → `/data/` (all three
containers) so the SQLite ledger survives restarts and rebuilds.

### 1.5 Updating after a code change

```sh
git pull
docker compose up -d --build
```

Compose rebuilds only images whose sources changed and restarts only
the affected containers. Stratum drops connections briefly on
restart — miners auto-reconnect within seconds.

### 1.6 What Docker does NOT install

You still need to install and run **bitcoind-patched**, **electrs**,
**bip300301_enforcer**, and (for PPS modes) **Thunder** somewhere the
containers can reach — see Part 4.

Skip to Part 4 next.

---

## Part 2 — Automated Ubuntu 24.04 deploy (Path B)

For a fresh box you own. `scripts/deploy-to-server.sh` handles
package installs, cloning, building, initializing SQLite, installing
systemd units, and (optionally) setting up nginx + ufw.

### 2.1 From your local machine

```sh
git clone https://github.com/rsantacroce/simplepool.git
cd simplepool

./scripts/deploy-to-server.sh \
    --host     user@pool.example.com \
    --root     /home/user/simplepool \
    --hostname pool.example.com \
    --ssh-key  ~/.ssh/your_key
```

The script is **idempotent** — re-run it after every `git push` to
redeploy.

### 2.2 What it does, step by step

1. `git fetch && git reset --hard origin/main` on the remote checkout
   (creates it on first run).
2. `apt-get install` — build tools, Node.js 20 (via NodeSource),
   sqlite3, nginx, ufw.
3. `make` the C proxy.
4. `npm ci --omit=dev` in `dashboard/` and `payout/`.
5. Initialize `data/shares.db` from `schema.sql` if missing.
6. Run the one-shot ms→seconds timestamp migration (harmless no-op on
   fresh installs).
7. Render the three systemd unit templates from
   [deploy/systemd/](deploy/systemd/) with the right `USER` / `ROOT`
   values, install to `/etc/systemd/system/`, `enable --now` each.
8. Drop the nginx vhost from [deploy/nginx/](deploy/nginx/) into
   `sites-available`, symlink to `sites-enabled`, `nginx -t && reload`.
   Open ports 80 / 443 / 3334 via `ufw` if active.

### 2.3 After it runs

You still need to:

- Install the drivechain infra (Part 4) — the script doesn't install
  bitcoind / electrs / enforcer / Thunder
- Configure `proxy.conf` for your chosen mode (Part 5)
- Set the admin password + wire the write-action env vars (Part 6)
- Configure the payout worker (Part 7, PPS modes only)

Skip to Part 4 next.

---

## Part 3 — Manual step-by-step (Path C)

For any Linux distro, or when you want to know what every step does.

### 3.1 Prerequisites

**Ubuntu 24.04 / Debian 12:**

```sh
sudo apt update
sudo apt install -y \
    build-essential \
    libsqlite3-dev libcurl4-openssl-dev libhiredis-dev \
    sqlite3 curl unzip \
    nginx-light ufw ca-certificates

# Node.js 20 (from NodeSource so you get a modern version)
curl -fsSL https://deb.nodesource.com/setup_20.x | sudo bash -
sudo apt install -y nodejs

# grpcurl — used by the admin Deposit action AND by any manual gRPC calls
GRPCURL_VER=1.9.3
curl -fsSL https://github.com/fullstorydev/grpcurl/releases/download/v${GRPCURL_VER}/grpcurl_${GRPCURL_VER}_linux_x86_64.tar.gz \
    | sudo tar -xz -C /usr/local/bin grpcurl
sudo chmod +x /usr/local/bin/grpcurl
```

**Fedora / RHEL 9 (dnf):**

```sh
sudo dnf install -y \
    gcc gcc-c++ make git \
    sqlite-devel libcurl-devel hiredis-devel \
    sqlite unzip curl \
    nginx firewalld

# Node.js 20 from NodeSource
curl -fsSL https://rpm.nodesource.com/setup_20.x | sudo bash -
sudo dnf install -y nodejs

# grpcurl — same tarball as above
GRPCURL_VER=1.9.3
curl -fsSL https://github.com/fullstorydev/grpcurl/releases/download/v${GRPCURL_VER}/grpcurl_${GRPCURL_VER}_linux_x86_64.tar.gz \
    | sudo tar -xz -C /usr/local/bin grpcurl
sudo chmod +x /usr/local/bin/grpcurl
```

### 3.2 Create a service user

Don't run as root. All three simplepool processes will run as this
user; the drivechain infra usually gets its own user too.

```sh
sudo adduser --system --group --home /home/simplepool --shell /bin/bash simplepool
sudo mkdir -p /home/simplepool/data /home/simplepool/logs
sudo chown -R simplepool:simplepool /home/simplepool
```

### 3.3 Clone + build the C proxy

```sh
sudo -u simplepool -H bash -lc '
  cd /home/simplepool
  git clone https://github.com/rsantacroce/simplepool.git .
  make -j"$(nproc)"
'
```

Binary lands at `/home/simplepool/build/simplepool`. Build with
`-Werror` — must be clean.

Run the tests:

```sh
sudo -u simplepool -H bash -lc 'cd /home/simplepool && make test'
```

All 7 suites must pass — `test_share`, `test_bitcoind`, `test_stratum`,
`test_store`, `test_coinbase`, `test_broadcast`, `test_thunder`.

### 3.4 Install Node dependencies

```sh
sudo -u simplepool -H bash -lc '
  cd /home/simplepool/dashboard && npm ci --omit=dev --no-fund --no-audit
  cd /home/simplepool/payout    && npm ci --omit=dev --no-fund --no-audit
'
```

`better-sqlite3` compiles a native binding — needs `build-essential`
+ `python3` (both installed above).

### 3.5 Initialize the SQLite database

```sh
sudo -u simplepool sqlite3 /home/simplepool/data/shares.db \
    < /home/simplepool/schema.sql
```

WAL mode is set by simplepool on first open — no manual pragma needed.

### 3.6 Install systemd units

Unit templates in [deploy/systemd/](deploy/systemd/) use `@USER@` and
`@ROOT@` placeholders. Render + install:

```sh
for u in simplepool.service simplepool-dashboard.service simplepool-payout.service; do
    sudo sed -e "s|@USER@|simplepool|g" -e "s|@ROOT@|/home/simplepool|g" \
        /home/simplepool/deploy/systemd/$u \
        | sudo tee /etc/systemd/system/$u >/dev/null
done
sudo systemctl daemon-reload
```

Do **not** `enable --now` yet — the units reference `proxy.conf` and
admin env vars that don't exist yet.

---

## Part 4 — drivechain infrastructure

Skip this section only if you're running `pool_mode=solo` against a
plain bitcoind. Otherwise all four services (bitcoind-patched,
electrs, enforcer, Thunder) need to be running before simplepool
starts.

### 4.1 bitcoind-patched (BIP300/301-aware)

**Download** the appropriate prebuilt:

- Linux x86_64: check <https://releases.drivechain.info/> for the
  current `L1-bitcoin-patched-latest-x86_64-linux.zip`
- Or build from source: <https://github.com/LayerTwo-Labs/bitcoin-patched>

Config skeleton — save as `/home/forknet/.drivechain-forknet/bitcoin.conf`
or wherever fits your file layout:

```
server=1
listen=1
txindex=1
rest=1
fallbackfee=0.0001

[main-or-signet-or-regtest-or-forknet]
rpcuser=<generate: openssl rand -hex 12>
rpcpassword=<generate: openssl rand -hex 20>
rpcport=8332
zmqpubrawblock=tcp://127.0.0.1:29000
zmqpubsequence=tcp://127.0.0.1:29000
```

**`rest=1` is required by the enforcer** — it queries the REST
interface at boot. Skipping this makes the enforcer exit with a clear
error at startup.

Launch (adapt paths + user):

```sh
sudo -u forknet -H bash -lc '
  ~/bitcoin-patched/bin/bitcoind -daemon
'
```

Wait for the initial block sync — days for mainnet, minutes for
forknet/signet. Check progress:

```sh
bitcoin-cli getblockchaininfo | jq '{blocks, headers, ibd:.initialblockdownload, verified:.verificationprogress}'
```

### 4.2 electrs

Powers the enforcer's `--wallet-esplora-url`. Prebuilt from
<https://releases.drivechain.info/> or build from
<https://github.com/mempool/electrs>.

Launch:

```sh
sudo -u forknet -H bash -lc '
  electrs \
    --network <same as bitcoind> \
    --daemon-dir /home/forknet/.drivechain-forknet \
    --daemon-rpc-addr 127.0.0.1:8332 \
    --cookie "<rpcuser>:<rpcpassword>" \
    --db-dir /home/forknet/electrs-db \
    --electrum-rpc-addr 127.0.0.1:50001 \
    --http-addr 127.0.0.1:3000 \
    --jsonrpc-import \
    --timestamp
'
```

### 4.3 bip300301_enforcer

Prebuilt from <https://releases.drivechain.info/>. Launch:

```sh
sudo -u forknet -H bash -lc '
  bip300301_enforcer \
    --node-rpc-addr=localhost:8332 \
    --node-rpc-user=<rpcuser> \
    --node-rpc-pass=<rpcpassword> \
    --bitcoin-core-skip-version-check \
    --enable-mempool --enable-wallet --wallet-auto-create \
    --wallet-esplora-url=http://127.0.0.1:3000
'
```

Exposed ports:

- `:8122` — `getblocktemplate` (what simplepool talks to)
- `:8123` — JSON-RPC (limited surface)
- `:50051` — gRPC (used by the deposit action, admin BTC-balance probe,
  and any manual `grpcurl` calls)

### 4.4 Thunder daemon (PPS modes only)

Prebuilt from <https://releases.drivechain.info/> — look for
`L2-S9-Thunder-latest-<arch>.zip`. Or build from
<https://github.com/LayerTwo-Labs/thunder-rust>.

Launch **after** the enforcer's gRPC on `:50051` is up:

```sh
sudo -u forknet -H bash -lc '
  thunder_app \
    --headless \
    --datadir /home/forknet/thunder-data \
    --network <same as your enforcer> \
    --mainchain-grpc-url http://127.0.0.1:50051 \
    --net-addr 127.0.0.1:4009 \
    --rpc-addr 127.0.0.1:6009 \
    --log-level INFO
'
```

On first boot, generate a wallet + address:

```sh
MNEMONIC=$(thunder_app_cli generate-mnemonic)
echo "SAVE THIS OFF-BOX: $MNEMONIC"
thunder_app_cli set-seed-from-mnemonic "$MNEMONIC"
POOL_ADDR=$(thunder_app_cli get-new-address)
echo "POOL RESERVE ADDRESS: $POOL_ADDR"
```

Copy `POOL_ADDR` — you'll set it as `pool_thunder_reserve_address` in
`proxy.conf` (Part 5) and as `THUNDER_FROM_ADDRESS` in the payout
worker unit (Part 7).

### 4.5 Thunder `mine` timer (recommended)

Thunder mints sidechain blocks only when its `mine` RPC is called.
Without a scheduler, deposits sit unclaimed until you call it manually.
Install a systemd timer that fires every 2 min:

```sh
sudo tee /usr/local/libexec/thunder-mine.sh >/dev/null <<'SCRIPT'
#!/usr/bin/env bash
set -uo pipefail
RPC=http://127.0.0.1:6009/
BODY='{"jsonrpc":"2.0","id":1,"method":"mine","params":[]}'
TS=$(date -u +%FT%TZ)
R=$(curl -sS --max-time 10 -H 'content-type: application/json' --data "$BODY" "$RPC" 2>&1)
echo "$TS  thunder-mine: $R"
exit 0
SCRIPT
sudo chmod +x /usr/local/libexec/thunder-mine.sh

sudo tee /etc/systemd/system/thunder-mine.service >/dev/null <<'UNIT'
[Unit]
Description=Nudge Thunder to mint a sidechain block (calls RPC `mine`)
After=network.target

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/thunder-mine.sh
StandardOutput=append:/home/forknet/logs/thunder-mine.log
StandardError=append:/home/forknet/logs/thunder-mine.log
UNIT

sudo tee /etc/systemd/system/thunder-mine.timer >/dev/null <<'UNIT'
[Unit]
Description=Call Thunder `mine` every 2 minutes to keep sidechain blocks flowing

[Timer]
OnBootSec=45s
OnUnitActiveSec=2min
AccuracySec=15s
Unit=thunder-mine.service

[Install]
WantedBy=timers.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable --now thunder-mine.timer
```

The `mine` RPC often returns `"BMM request with same prev_bytes
already exists"` when there's nothing new to commit — that's fine
and gets logged harmlessly.

---

## Part 5 — configuring `proxy.conf`

```sh
sudo -u simplepool cp /home/simplepool/proxy.conf.example \
                     /home/simplepool/proxy.conf
sudo -u simplepool $EDITOR /home/simplepool/proxy.conf
```

Every key is documented inline in the example. Minimum-useful configs
per mode below.

### 5.1 Solo mode

```conf
listen_addr = 0.0.0.0
listen_port = 3334
# 0 disables the reaper (legacy blocking recv); default 600s (10 min)
# is a good middle ground.
idle_timeout_sec = 600

bitcoind_url = http://127.0.0.1:8332
bitcoind_user = <rpcuser>
bitcoind_pass = <rpcpassword>

operator_address = bc1q...        # your BTC wallet for the 1% fee
fee_bps          = 100
coinbase_tag     = /your-pool/

pool_mode = solo                  # default; can be omitted

initial_diff       = 1
vardiff_enabled    = 1
vardiff_target_spm = 12
vardiff_min        = 1
vardiff_max        = 1e12
vardiff_window_sec = 30

db_path   = /home/simplepool/data/shares.db
log_level = info
```

Miner username format: `<their BTC address>[.<rig_label>]`.

### 5.2 PPS mode (byte-shape validation only)

**Not useful for real payouts.** Only if you're testing the coinbase
byte layout.

```conf
# ... all of solo, plus:
pool_mode                    = pps
pool_thunder_reserve_address = <base58 Thunder address from Part 4.4>
thunder_sidechain_number     = 9
pps_sats_per_diff            = 1000
# optional; overrides the OP_RETURN payload
# thunder_op_return_hex =
```

Miner username: `<their Thunder address>[.<rig_label>]`. Startup logs
a WARN — intentional.

### 5.3 PPS-classic mode (recommended for real Thunder payouts)

```conf
# ... all of solo, plus:
pool_mode                    = pps-classic
pool_btc_address             = bc1q...   # pool BTC wallet — ideally an
                                         #   enforcer-owned address so
                                         #   the deposit flow can spend it
pool_thunder_reserve_address = <base58 Thunder address from Part 4.4>
pps_sats_per_diff            = 1000
```

Miner username: `<their Thunder address>[.<rig_label>]`. Startup logs
`pool_mode=pps-classic: pool_btc_address=…`.

### 5.4 Optional: Redis broadcast

Any mode. Publishes every share / reject / block / tip / PPS credit to
Redis pub/sub for downstream consumers.

```conf
redis_url                  = redis://127.0.0.1:6379
redis_publish_timeout_ms   = 200
redis_reconnect_backoff_ms = 2000
```

Channels: `pool:shares`, `pool:rejects`, `pool:blocks`, `pool:tip`,
`pool:credits`.

---

## Part 6 — dashboard configuration

The dashboard is a Node process serving both the public dashboard
(`/`, `/blocks`, `/worker/:name`) and the operator admin surface
(`/admin/*`, HTTP Basic gated). New in the current build: the admin
surface has **write actions** — you can trigger payouts, submit
deposits, remove stuck Thunder txs, and nudge `mine` all from the
browser.

### 6.1 Generate an admin password

```sh
openssl rand -base64 21 | tr -d '=+/' \
    | sudo tee /root/simplepool-admin-cred.txt
sudo chmod 600 /root/simplepool-admin-cred.txt
cat /root/simplepool-admin-cred.txt   # copy this for the drop-in below
```

Do **not** commit or share this file.

### 6.2 Install a dashboard drop-in

The shipped [`deploy/systemd/simplepool-dashboard.service`](deploy/systemd/simplepool-dashboard.service)
sets sane defaults. Override deployment-specific things with a
drop-in so you don't fork the shipped template:

```sh
sudo mkdir -p /etc/systemd/system/simplepool-dashboard.service.d
sudo tee /etc/systemd/system/simplepool-dashboard.service.d/local.conf <<'CONF'
[Service]
# --- Public "Connect a miner" card ---
Environment=PUBLIC_STRATUM_URL=stratum+tcp://pool.example.com:3334

# --- Admin panel gate (omit to keep /admin returning 503) ---
Environment=ADMIN_USER=admin
Environment=ADMIN_PASSWORD=<paste from /root/simplepool-admin-cred.txt>

# --- Data sources for the admin read views ---
Environment=THUNDER_RPC_URL=http://127.0.0.1:6009
Environment=POOL_THUNDER_RESERVE_ADDRESS=<same as proxy.conf reserve address>
Environment=POOL_PPS_SATS_PER_DIFF=1000

# --- Write actions (POST buttons on /admin/{deposits,payouts,tools}) ---
# Payout worker's admin HTTP surface (see Part 7).
Environment=PAYOUT_ADMIN_URL=http://127.0.0.1:9080
# Enforcer gRPC — the Deposit action shells out to grpcurl.
Environment=ENFORCER_GRPC_ADDR=127.0.0.1:50051
Environment=GRPCURL_BIN=/usr/local/bin/grpcurl
Environment=THUNDER_SIDECHAIN_ID=9

# --- Data dir must be writable (the deposit action logs to the DB) ---
ReadWritePaths=/home/simplepool/data
CONF
sudo systemctl daemon-reload
```

### 6.3 The admin surface at a glance

Five sub-pages behind Basic auth, distinct orange top-nav:

| tab | URL | contents |
|---|---|---|
| Overview | `/admin` | Thunder reserve, Pool BTC (enforcer wallet), PPS totals, recent blocks |
| Workers | `/admin/workers` | Per-worker balance table + audit links |
| Deposits | `/admin/deposits` | History table + New Deposit form (with live "Spendable now" hint) |
| Payouts | `/admin/payouts` | Recent + in-flight + Trigger button |
| Tools | `/admin/tools` | Nudge mine + Remove-from-mempool |
| Log out | `/admin/logout` | 401 → browser clears cached Basic creds |

All write actions are CSRF-gated: each POST carries a single-use
token issued on page render, redirects back to its own tab with a
green ✓ / red ✗ flash banner.

### 6.4 Reverse proxy (nginx + Let's Encrypt)

The dashboard binds `0.0.0.0:8081` — reachable directly, but do **not**
expose `/admin` on plain HTTP over the internet. Basic auth over plain
HTTP leaks the password on every request.

Shipped nginx template at
[`deploy/nginx/pool.drivechain.info.conf`](deploy/nginx/pool.drivechain.info.conf) —
substitute your hostname, drop into `sites-available`, symlink, reload:

```sh
sudo sed 's/pool\.drivechain\.info/your.pool.example.com/g' \
    /home/simplepool/deploy/nginx/pool.drivechain.info.conf \
    | sudo tee /etc/nginx/sites-available/your.pool.example.com.conf
sudo ln -sf /etc/nginx/sites-available/your.pool.example.com.conf \
            /etc/nginx/sites-enabled/
sudo cp /home/simplepool/deploy/nginx/pool-ratelimit.conf \
        /etc/nginx/conf.d/
sudo nginx -t && sudo systemctl reload nginx

# TLS via certbot
sudo apt install -y certbot python3-certbot-nginx
sudo certbot --nginx -d your.pool.example.com
```

**Stratum stays plain TCP** — it's not HTTP, does not go through
nginx. Miners connect directly to `<your-host>:3334`. Point your
stratum hostname at the box's IP via DNS.

### 6.5 Firewall (ufw)

```sh
sudo ufw allow OpenSSH
sudo ufw allow 80/tcp     # HTTP (Let's Encrypt)
sudo ufw allow 443/tcp    # HTTPS (dashboard)
sudo ufw allow 3334/tcp   # stratum
sudo ufw enable
```

Do **not** open `:8081` (dashboard), `:9080` (payout admin HTTP),
`:6009` (Thunder RPC), `:50051` (enforcer gRPC), or `:8332`
(bitcoind RPC) to the internet. Nginx fronts the dashboard; everything
else is loopback-only.

---

## Part 7 — payout worker (PPS modes only)

Drains `pps_credits.accrued_sats - paid_sats` by issuing Thunder txs.
Also runs a loopback-only HTTP admin surface (`:9080/tick`) that the
dashboard's "Trigger payout now" button POSTs to.

### 7.1 Dry-run first

Before enabling the service, verify the config works and see exactly
what would be paid:

```sh
sudo -u simplepool -H bash -lc '
  cd /home/simplepool/payout &&
  PAYOUT_DRY_RUN=1 \
  PAYOUT_DB_PATH=/home/simplepool/data/shares.db \
  THUNDER_RPC_URL=http://127.0.0.1:6009 \
  THUNDER_FROM_ADDRESS=any \
  PAYOUT_INTERVAL_MS=2000 \
  PAYOUT_ADMIN_PORT=0 \
  node index.js
'
```

Log lines like `payout: DRY <worker> -> <address> N sats` show what the
worker WOULD send. Ctrl-C when satisfied.

### 7.2 Install a drop-in

```sh
sudo mkdir -p /etc/systemd/system/simplepool-payout.service.d
sudo tee /etc/systemd/system/simplepool-payout.service.d/local.conf <<'CONF'
[Service]
# Must equal proxy.conf's pool_thunder_reserve_address — the wallet
# the worker spends from.
Environment=THUNDER_FROM_ADDRESS=<paste here>

# Admin HTTP surface — the dashboard's "Trigger payout now" button
# POSTs to http://127.0.0.1:9080/tick. Loopback-only; set port=0 to
# disable entirely.
Environment=PAYOUT_ADMIN_BIND=127.0.0.1
Environment=PAYOUT_ADMIN_PORT=9080

# Defaults; override if you want:
# Environment=PAYOUT_MIN_SATS=10000        # skip workers below this
# Environment=PAYOUT_INTERVAL_MS=30000     # tick every 30s
# Environment=PAYOUT_MAX_PER_TICK=50       # cap per tick
CONF
sudo systemctl daemon-reload
```

### 7.3 Enable and tail

```sh
sudo systemctl enable --now simplepool-payout.service
sudo journalctl -u simplepool-payout.service -f
```

Expected first lines:

```
simplepool-payout starting (db=/home/.../shares.db rpc=http://127.0.0.1:6009)
  interval=30000ms min_sats=10000 max_per_tick=50
payout admin http listening on 127.0.0.1:9080
```

Then either `payout: N due, ...` (if there's work) or
`payout: no due workers` (silent at debug level) every 30s.

If the Thunder reserve is empty, expect:

```
payout: reserve short — available=0 needed=N; partial payouts disabled this tick
```

That's the intended "fail safe" — funds the reserve via the admin
Deposits tab, then it clears on the next tick.

---

## Part 8 — first-run verification

Start everything (order matters for PPS modes — start Thunder + enforcer
first, then simplepool + dashboard + payout):

```sh
sudo systemctl enable --now simplepool.service
sudo systemctl enable --now simplepool-dashboard.service
sudo systemctl enable --now simplepool-payout.service    # PPS modes only
sudo systemctl status simplepool simplepool-dashboard simplepool-payout
```

Then work through this checklist:

### 8.1 Ports are listening

```sh
ss -tlnp | grep -E ':(3334|8081|9080)\s'
# expect:
#   :3334  simplepool         (stratum)
#   :8081  node (dashboard)
#   :9080  node (payout admin) — 127.0.0.1 only
```

### 8.2 Pool talks to bitcoind / enforcer

```sh
sudo journalctl -u simplepool -n 20 --no-pager
# look for:  "new job: height=... prev=... txs=..."
# (one line per template refresh; ~ every 5s at log_level=info)
```

### 8.3 Dashboard reachable

```sh
curl -sSf http://127.0.0.1:8081/         >/dev/null && echo public   ok
curl -sSf http://127.0.0.1:8081/healthz  >/dev/null && echo healthz  ok
```

### 8.4 Admin panel gated + reachable

```sh
# Should 401 without auth
curl -sS -o /dev/null -w "no auth: %{http_code}\n" \
    http://127.0.0.1:8081/admin

# Should 200 with auth
ADMIN_PASS=$(sudo cat /root/simplepool-admin-cred.txt)
curl -sS -o /dev/null -w "with pw: %{http_code}\n" \
    -u "admin:$ADMIN_PASS" http://127.0.0.1:8081/admin

# All 5 sub-pages should render
for p in / /workers /deposits /payouts /tools; do
    code=$(curl -sS -u "admin:$ADMIN_PASS" -o /dev/null -w "%{http_code}" \
        "http://127.0.0.1:8081/admin$p")
    printf "  %-14s -> %s\n" "/admin$p" "$code"
done
```

### 8.5 Payout worker admin HTTP responds

```sh
curl -sS http://127.0.0.1:9080/healthz
# {"ok":true}

curl -sS -X POST http://127.0.0.1:9080/tick
# {"ok":true,"result":{"attempted":0,"paid":0,"failed":0}}
```

### 8.6 First miner authorizes and submits a share

Point a real miner at `stratum+tcp://<your-host>:3334` with the correct
username format (BTC address for solo, Thunder address for pps /
pps-classic). Watch shares arrive:

```sh
sudo journalctl -u simplepool -f | grep SUBMIT
```

The `[SUBMIT CHECK]` line shows sent hash, worker target, and network
target — see [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md#the-share-validation-flow).

### 8.7 Accrual is landing (PPS modes only)

```sh
sudo -u simplepool sqlite3 /home/simplepool/data/shares.db \
    'SELECT * FROM pps_credits'
```

Should show a row per miner with a growing `accrued_sats`.

### 8.8 Audit cross-check green

Open `https://your.pool.example.com/admin`, log in, click **Workers**,
then `audit →` on a worker's row. The **Cross-check** card should
show ✓ — meaning stored `accrued_sats` matches `Σ FLOOR(diff × rate)`.
If it shows ⚠, the `POOL_PPS_SATS_PER_DIFF` env var on the dashboard
doesn't match `pps_sats_per_diff` in `proxy.conf` — fix and restart
the dashboard.

---

## Part 9 — local dev (macOS or Linux)

For contributors or anyone wanting to touch every code path locally.

### 9.1 macOS prereqs (Homebrew)

```sh
brew install sqlite curl hiredis node grpcurl
xcode-select --install    # if `cc` isn't available
```

Linux prereqs are the same as Part 3.1.

### 9.2 Clone + build + test

```sh
git clone https://github.com/rsantacroce/simplepool.git
cd simplepool
make -j"$(nproc)"
make test
```

### 9.3 Regtest stack

If you want a full drivechain stack locally and to see a real block
get mined by simplepool, [`scripts/regtest/`](scripts/regtest/) has
everything:

```sh
scripts/regtest/setup.sh                # downloads prebuilts
scripts/regtest/start.sh                # brings up all four processes
scripts/regtest/status.sh               # ps-style summary
scripts/regtest/activate-thunder.sh     # proposes + acks sidechain #9
scripts/regtest/thunder-init.sh         # generates a Thunder wallet
scripts/regtest/validate.sh             # bootstraps 150 blocks, probes GBT
node scripts/regtest/cpuminer.js --timeout 60   # mine a block via the pool
scripts/regtest/inspect-coinbase.sh
scripts/regtest/stop.sh
```

Full details in [scripts/regtest/README.md](scripts/regtest/README.md).
Everything runs under `.regtest/` (gitignored) — safe to re-run.

The regtest stack matches the production stack process-for-process
(same enforcer, same electrs, same Thunder), so if your changes work
against it, they'll work against forknet or mainnet-drivechain.

---

## Part 10 — backups + monitoring

### 10.1 SQLite backup

The ledger (`data/shares.db`) is the only piece of pool state that
can't be regenerated from external sources. Back it up:

```sh
# Add to /etc/cron.d/simplepool-backup
0 * * * * simplepool /usr/bin/sqlite3 /home/simplepool/data/shares.db ".backup /home/simplepool/data/shares.snapshot.db"
```

The `.backup` command is safe against WAL — you'll get a consistent
snapshot even while simplepool + payout are writing.

Sync the snapshot off-box (rsync, restic, borg, whatever). Never
copy the live `shares.db` file directly — that races the WAL.

### 10.2 Log rotation

`journalctl` handles simplepool + dashboard + payout logs by default.
For file-based logs (thunder daemon, enforcer, thunder-mine timer):

```sh
sudo tee /etc/logrotate.d/simplepool-forknet <<'ROT'
/home/forknet/logs/*.log {
    daily
    rotate 14
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
ROT
```

### 10.3 What to monitor

At minimum:

- `systemctl is-active simplepool simplepool-dashboard simplepool-payout` (host)
- `systemctl is-active thunder-mine.timer` (Thunder mint scheduler)
- Stratum port `:3334` reachable from the internet
- Dashboard `:8081/healthz` returns `{ok:true, db_ready:true}`
- Payout admin `:9080/healthz` returns `{ok:true}`
- `bitcoin-cli getblockchaininfo | jq .blocks` advancing
- Free disk space (bitcoind txindex grows unbounded)

---

## Part 11 — troubleshooting

Also see [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md#troubleshooting) for
runtime issues. Install-time gotchas below:

### Build fails with `hiredis/hiredis.h: No such file or directory`

libhiredis dev headers aren't installed. See Part 3.1 (`libhiredis-dev`
on Debian, `hiredis-devel` on Fedora).

### Pool exits at boot with `initial GBT failed: curl: Could not connect`

bitcoind or the enforcer isn't running / not reachable at the URL in
`proxy.conf`. Verify:

```sh
curl -u <rpcuser>:<rpcpassword> --data-binary \
    '{"jsonrpc":"1.0","id":"probe","method":"getblockchaininfo","params":[]}' \
    -H 'content-type: text/plain;' \
    http://127.0.0.1:8332/
```

### Enforcer exits with `Bitcoin Core REST server is not enabled`

`rest=1` missing from `bitcoin.conf`.

### Thunder exits with `Unable to verify existence of CUSF mainchain service(s)`

Thunder started before the enforcer's gRPC (`:50051`) was ready. If
you're using systemd, add `After=bip300301-enforcer.service` and
`Requires=` to your Thunder unit; if running by hand, sleep 5s.

### `config error: 'pool_btc_address' is required when pool_mode=pps-classic`

Self-explanatory; set it (Part 5.3).

### `stratum bind 0.0.0.0:3334: Address already in use`

An old simplepool is still running. Match it by **exact process name** —
a `pkill -f simplepool` from an SSH session will kill your own shell
because SSH's command line contains the string "simplepool":

```sh
sudo pkill -9 -x simplepool
```

### Miner authorize is rejected with `invalid thunder address`

In `pps` / `pps-classic` mode the username must be a Thunder base58
address (or the wrapper `s9_<base58>_<hex6>` — which the pool now
explicitly rejects; use the bare form). See
[NONCE_AND_SHARES.md](NONCE_AND_SHARES.md#extranonce1-allocation--how-uniqueness-is-guaranteed).

### Docker build fails: `libhiredis1.1.0` not found

Debian bookworm ships `libhiredis0.14`, not `libhiredis1.1.0`. Fixed
in the shipped Dockerfile.

### Docker build fails downloading `grpcurl`

The dashboard Dockerfile fetches grpcurl v1.9.1 from GitHub releases
during build. Check network + GitHub reachability from your build host.

### Admin `Deposit` action returns `db.prepare is not a function`

Older bug, fixed. Pull `admin-nav-refactor` or newer; the dashboard
now opens a separate writable SQLite handle.

### `/admin` POST redirects show `flash=CSRF token missing or already used`

Every CSRF token is single-use with a 1h TTL. Refresh the admin page
to get a fresh token, then retry.

### Payout worker keeps logging `utxo double spent`

Thunder's mempool has a phantom tx pinning the reserve UTXO. From the
admin **Tools** tab, use **Remove stale Thunder tx** with the offending
txid (grep the payout worker log for the most recent broadcast-attempt
txid). Thunder's mempool is persisted on disk — a daemon restart alone
doesn't clear it, but `remove_from_mempool` does.

---

## Part 12 — uninstall / cleanup

Bare-metal:

```sh
# Stop and disable services
sudo systemctl disable --now simplepool simplepool-dashboard simplepool-payout \
                             thunder-mine.timer

# Remove unit files
sudo rm -f /etc/systemd/system/simplepool*.service \
           /etc/systemd/system/thunder-mine.{service,timer} \
           /etc/systemd/system/simplepool-*.service.d/local.conf
sudo rmdir /etc/systemd/system/simplepool-*.service.d 2>/dev/null || true
sudo systemctl daemon-reload

# Remove nginx vhost
sudo rm -f /etc/nginx/sites-enabled/*pool*.conf \
           /etc/nginx/sites-available/*pool*.conf \
           /etc/nginx/conf.d/pool-ratelimit.conf
sudo nginx -t && sudo systemctl reload nginx

# Delete data + user (destructive — back up shares.db first!)
sudo cp /home/simplepool/data/shares.db /root/shares.db.final-backup
sudo deluser --remove-home simplepool

# Optional: remove grpcurl
sudo rm -f /usr/local/bin/grpcurl /usr/local/libexec/thunder-mine.sh
sudo rm -f /root/simplepool-admin-cred.txt
```

Docker:

```sh
cd simplepool/deploy/docker
docker compose down --volumes --remove-orphans
docker image rm simplepool:latest simplepool-dashboard:latest simplepool-payout:latest
# data/ stays on the host — back it up or delete manually
```

---

## Where to go next

- Running the pool day-to-day: [OPERATOR_GUIDE.md](OPERATOR_GUIDE.md)
- Auditing the numbers: [NONCE_AND_SHARES.md](NONCE_AND_SHARES.md)
- Design rationale for pps-classic vs pps:
  [CLASSIC_PAYOUTS.md](CLASSIC_PAYOUTS.md), [PPS_THUNDER.md](PPS_THUNDER.md)
- Regtest full walk-through: [scripts/regtest/README.md](scripts/regtest/README.md)
- Branch-level test plan: [VERIFY.md](VERIFY.md)
- Docker specifics: [deploy/docker/README.md](deploy/docker/README.md)
