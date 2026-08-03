// db.js - SQLite storage for saved queries and dashboards.
//
// SQLite (file-based, zero setup) rather than a real database server on
// the theory that this scaffold's persistence needs are small (users,
// queries, dashboards - no high write volume, nothing the compute path
// depends on). Swap for Postgres/etc. behind the same getDb()-returns-a-
// query-interface shape if that stops being true.

import Database from 'better-sqlite3';
import { mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const DB_PATH = process.env.WC_DB_PATH || join(__dirname, '..', 'data', 'web-csv.sqlite');

const SCHEMA = `
CREATE TABLE IF NOT EXISTS users (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	email TEXT UNIQUE NOT NULL,
	created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS queries (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	owner_id INTEGER NOT NULL REFERENCES users(id),
	name TEXT NOT NULL,
	grammar_path TEXT NOT NULL,
	source TEXT NOT NULL,
	created_at TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE TABLE IF NOT EXISTS dashboards (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	owner_id INTEGER NOT NULL REFERENCES users(id),
	name TEXT NOT NULL,
	layout TEXT NOT NULL,
	created_at TEXT NOT NULL DEFAULT (datetime('now'))
);
`;

let db = null;

export function getDb() {
	if (!db) {
		mkdirSync(dirname(DB_PATH), { recursive: true });
		db = new Database(DB_PATH);
		db.pragma('journal_mode = WAL');
		db.exec(SCHEMA);
	}
	return db;
}
