// client.js - talks to server/ for the persistence features (saved
// queries, saved dashboards) the compute path itself doesn't need. See
// ARCHITECTURE.md's "deployment model" note: everything in gpu/ and
// csv/ works with no backend at all; this is the one part of the app
// that does need one.

const API_BASE = '/api';

async function asJson(res, what) {
	if (!res.ok) {
		throw new Error(`${what} failed: ${res.status} ${res.statusText}`);
	}
	return res.json();
}

export async function listQueries() {
	return asJson(await fetch(`${API_BASE}/queries`), 'listQueries');
}

export async function saveQuery({ name, grammarPath, source }) {
	const res = await fetch(`${API_BASE}/queries`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ name, grammarPath, source }),
	});
	return asJson(res, 'saveQuery');
}

export async function listDashboards() {
	return asJson(await fetch(`${API_BASE}/dashboards`), 'listDashboards');
}

export async function saveDashboard({ name, layout }) {
	const res = await fetch(`${API_BASE}/dashboards`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({ name, layout }),
	});
	return asJson(res, 'saveDashboard');
}
