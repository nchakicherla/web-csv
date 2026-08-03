// client.js - talks to server/ for the persistence features (saved
// queries, saved dashboards) the compute path itself doesn't need. See
// ARCHITECTURE.md's "deployment model" note: everything in gpu/ and
// csv/ works with no backend at all; this is the one part of the app
// that does need one.

const API_BASE = '/api';

// The server's auth (server/src/routes/auth.js) is a dev-only stub that
// trusts an x-user-id header as-is - there's no real login to get an
// identity from yet. A stable per-browser id (not tied to any account)
// is enough to make requests succeed instead of 401ing, which is all this
// scaffold needs; replace this with whatever a real login flow sets once
// one exists, at the same time auth.js itself gets replaced.
function getDevUserId() {
	let id = localStorage.getItem('wc-dev-user-id');
	if (!id) {
		id = `dev-${Math.random().toString(36).slice(2)}@localhost`;
		localStorage.setItem('wc-dev-user-id', id);
	}
	return id;
}

function authHeaders(extra = {}) {
	return { 'x-user-id': getDevUserId(), ...extra };
}

async function asJson(res, what) {
	if (!res.ok) {
		throw new Error(`${what} failed: ${res.status} ${res.statusText}`);
	}
	return res.json();
}

export async function listQueries() {
	const res = await fetch(`${API_BASE}/queries`, { headers: authHeaders() });
	return asJson(res, 'listQueries');
}

export async function saveQuery({ name, grammarPath, source }) {
	const res = await fetch(`${API_BASE}/queries`, {
		method: 'POST',
		headers: authHeaders({ 'Content-Type': 'application/json' }),
		body: JSON.stringify({ name, grammarPath, source }),
	});
	return asJson(res, 'saveQuery');
}

export async function listDashboards() {
	const res = await fetch(`${API_BASE}/dashboards`, { headers: authHeaders() });
	return asJson(res, 'listDashboards');
}

export async function saveDashboard({ name, layout }) {
	const res = await fetch(`${API_BASE}/dashboards`, {
		method: 'POST',
		headers: authHeaders({ 'Content-Type': 'application/json' }),
		body: JSON.stringify({ name, layout }),
	});
	return asJson(res, 'saveDashboard');
}
