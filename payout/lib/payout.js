/* Payout loop.
 *
 * Each tick:
 *   1. SELECT workers from pps_credits where (accrued - paid) >= minSats.
 *   2. Check the Thunder reserve has enough available balance to cover
 *      the sum + a fee budget. If not, log and skip until next tick.
 *   3. For each due worker, call thunder.transfer(addr, owed, fee).
 *      On success, UPDATE pps_credits.paid_sats += owed.
 *      On failure, log and leave the row untouched — next tick retries.
 *
 * Failure isolation:
 *   - We do NOT batch payouts into a single tx. One miner's bad address
 *     or a transient RPC error must not block other miners' payouts.
 *   - If a Thunder tx is broadcast but our UPDATE fails, we'd
 *     double-pay on retry. That's acceptable rare-failure behaviour for
 *     the MVP; a stricter design would write a payouts_in_flight ledger
 *     keyed on (worker_id, txid) before broadcast and reconcile after.
 *     Left as a follow-up — flagged in payout/README.md. */

import { listDue, markPaid } from './db.js';

/* Fee model: flat per-tx fee, configurable later. Thunder is a sidechain
 * with relatively low fees; 100 sats covers a one-input one-output tx
 * comfortably on regtest and should be a sane default for early
 * deployments. Will revisit once mainnet fee dynamics are observable. */
const TX_FEE_SATS = 100n;

export async function runOnce(ctx, log) {
    const { db, thunder, cfg } = ctx;
    const due = listDue(db, { minSats: cfg.minSats, limit: cfg.maxPerTick });
    if (due.length === 0) {
        log.debug?.('payout: no due workers');
        return { attempted: 0, paid: 0, failed: 0 };
    }

    const totalOwed = due.reduce((a, r) => a + r.owed_sats, 0n);
    const totalFees = BigInt(due.length) * TX_FEE_SATS;
    log.info(`payout: ${due.length} due, total owed=${totalOwed} sats, fees~${totalFees}`);

    if (!cfg.dryRun) {
        let bal;
        try {
            bal = await thunder.balance();
        } catch (e) {
            log.warn(`payout: balance() failed (${e.message}); skipping tick`);
            return { attempted: 0, paid: 0, failed: 0 };
        }
        const avail = BigInt(bal.available_sats ?? bal.total_sats ?? 0);
        if (avail < totalOwed + totalFees) {
            log.warn(
                `payout: reserve short — available=${avail} needed=${totalOwed + totalFees}; ` +
                'partial payouts disabled this tick'
            );
            return { attempted: 0, paid: 0, failed: 0, reserve_short: true };
        }
    }

    const now_s = Math.floor(Date.now() / 1000);
    let paid = 0, failed = 0;
    for (const r of due) {
        if (cfg.dryRun) {
            log.info(`payout: DRY ${r.worker_name} -> ${r.thunder_address} ${r.owed_sats} sats`);
            continue;
        }
        try {
            const txid = await thunder.transfer(
                r.thunder_address, r.owed_sats, TX_FEE_SATS);
            markPaid(db, r.worker_id, r.owed_sats, now_s);
            log.info(`payout: ${r.worker_name} -> ${r.thunder_address} ` +
                     `${r.owed_sats} sats txid=${txid}`);
            paid++;
        } catch (e) {
            log.warn(`payout: ${r.worker_name} failed: ${e.message}`);
            failed++;
        }
    }
    return { attempted: due.length, paid, failed };
}

export function startLoop(ctx, log) {
    let stopped = false;
    let timer = null;

    const tick = async () => {
        if (stopped) return;
        try {
            await runOnce(ctx, log);
        } catch (e) {
            log.error(`payout: unexpected error: ${e.stack || e.message}`);
        }
        if (!stopped) timer = setTimeout(tick, ctx.cfg.intervalMs);
    };

    tick();

    return {
        stop() {
            stopped = true;
            if (timer) clearTimeout(timer);
        },
    };
}
