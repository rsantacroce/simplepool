// Read-only SQLite wrapper around the simplepool shares database.
//
// The dashboard MUST tolerate the database not yet existing — the C proxy
// may not have been started. We expose `get()` which lazily opens the file
// the first time it appears on disk and returns null until then.

import Database from 'better-sqlite3';
import fs from 'node:fs';

export function openDb(dbPath, { readonly = true } = {}) {
    let db = null;
    let lastTryMs = 0;

    function tryOpen() {
        if (db) return db;
        const now = Date.now();
        if (now - lastTryMs < 1000) return null; // throttle
        lastTryMs = now;
        if (!fs.existsSync(dbPath)) return null;
        try {
            db = new Database(dbPath, { readonly, fileMustExist: true });
            // WAL is required for read-consistency while the C proxy writes.
            db.pragma('journal_mode = WAL');
            db.pragma('busy_timeout = 2000');
        } catch (err) {
            console.error('[db] open failed:', err.message);
            db = null;
        }
        return db;
    }

    return {
        path: dbPath,
        get: tryOpen,
        ready: () => db !== null,
    };
}
