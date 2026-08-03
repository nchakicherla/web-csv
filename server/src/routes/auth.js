// auth.js - placeholder identity for local development only.
//
// Real auth (sessions, OAuth/SSO, whatever fits the deployment) is a
// deliberate TODO, not an oversight - it's a decision with real
// tradeoffs that shouldn't get made implicitly by scaffolding code. This
// middleware trusts an `x-user-id` header as a plain email string, as-is,
// auto-creating a user row for it if none exists yet. That's fine for one
// developer poking at the API on localhost and a real security hole on
// anything reachable by anyone else - replace this before deploying
// anywhere multi-tenant.

import { getDb } from '../db.js';

export function requireUser(req, res, next) {
	const email = req.header('x-user-id');
	if (!email) {
		res.status(401).json({
			error: 'missing x-user-id header (dev-only stub auth - see server/src/routes/auth.js)',
		});
		return;
	}

	const db = getDb();
	let user = db.prepare('SELECT * FROM users WHERE email = ?').get(email);
	if (!user) {
		const info = db.prepare('INSERT INTO users (email) VALUES (?)').run(email);
		user = { id: info.lastInsertRowid, email };
	}

	req.user = user;
	next();
}
