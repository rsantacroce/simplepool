/* Read-write SQLite wrapper for the payout worker.
 *
 * The C proxy is the only writer to pps_credits.accrued_sats — every
 * accepted share INSERTs/UPSERTs with `accrued_sats = accrued_sats + delta`
 * via the writer thread. This worker is the only writer to
 * pps_credits.paid_sats, and only ever after a Thunder transaction has
 * been broadcast successfully.
 *
 * Both writers serialise through WAL with a generous busy_timeout, so
 * brief lock contention during the proxy's batch commit is invisible. */

import Database from 'better-sqlite3';

export function openDb(dbPath) {
    const db = new Database(dbPath, { fileMustExist: true });
    db.pragma('journal_mode = WAL');
    db.pragma('synchronous  = NORMAL');
    db.pragma('busy_timeout = 5000');
    return db;
}

/* Workers with non-zero outstanding balance, joined to the workers table
 * so we have the Thunder address (stored in workers.payout_address by the
 * C proxy on first sight). Filters out workers whose owed balance is
 * below minSats. */
export function listDue(db, { minSats, limit }) {
    const rows = db.prepare(`
        SELECT  w.id              AS worker_id,
                w.name            AS worker_name,
                w.payout_address  AS thunder_address,
                c.accrued_sats    AS accrued_sats,
                c.paid_sats       AS paid_sats,
                (c.accrued_sats - c.paid_sats) AS owed_sats
        FROM    pps_credits c
        JOIN    workers     w ON w.id = c.worker_id
        WHERE   (c.accrued_sats - c.paid_sats) >= ?
          AND   w.payout_address IS NOT NULL
          AND   w.payout_address != ''
        ORDER BY owed_sats DESC
        LIMIT   ?
    `).all(Number(minSats), limit);
    return rows.map(r => ({ ...r, owed_sats: BigInt(r.owed_sats) }));
}

/* Mark a payout as settled. We INCREMENT paid_sats by deltaSats rather
 * than overwriting, in case the proxy has accrued more shares for this
 * worker between listDue() and this call — that's safe because accrued
 * only ever grows. */
export function markPaid(db, workerId, deltaSats, nowSec) {
    db.prepare(`
        UPDATE pps_credits
           SET paid_sats    = paid_sats + ?,
               last_updated = ?
         WHERE worker_id    = ?
    `).run(Number(deltaSats), nowSec, workerId);
}
