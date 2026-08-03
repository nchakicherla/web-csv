// api.test.js - drives the real Express app (server/src/app.js) on an
// ephemeral port against a throwaway SQLite file, via real HTTP requests -
// no mocking of Express or better-sqlite3. WC_DB_PATH must be set before
// app.js (and transitively db.js) is first imported, since db.js reads it
// once and caches the connection - see db.js's getDb().

import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const tmpDir = mkdtempSync(join(tmpdir(), 'web-csv-test-'));
process.env.WC_DB_PATH = join(tmpDir, 'test.sqlite');

const { app } = await import('../src/app.js');

let server;
let base;

before(() => {
	server = app.listen(0);
	base = `http://localhost:${server.address().port}`;
});

after(() => {
	server.close();
	rmSync(tmpDir, { recursive: true, force: true });
});

test('GET /api/queries without x-user-id returns 401', async () => {
	const res = await fetch(`${base}/api/queries`);
	assert.equal(res.status, 401);
});

test('GET /api/dashboards without x-user-id returns 401', async () => {
	const res = await fetch(`${base}/api/dashboards`);
	assert.equal(res.status, 401);
});

test('POST then GET /api/queries round-trips correctly', async () => {
	const headers = { 'Content-Type': 'application/json', 'x-user-id': 'alice@test' };
	const postRes = await fetch(`${base}/api/queries`, {
		method: 'POST',
		headers,
		body: JSON.stringify({
			name: 'big spenders',
			grammarPath: '/resources/grammar-csv.txt',
			source: 'emit(sum(col("amount")));',
		}),
	});
	assert.equal(postRes.status, 201);
	const { id } = await postRes.json();
	assert.equal(typeof id, 'number');

	const getRes = await fetch(`${base}/api/queries`, { headers });
	assert.equal(getRes.status, 200);
	const list = await getRes.json();
	assert.equal(list.length, 1);
	assert.equal(list[0].name, 'big spenders');
	assert.equal(list[0].grammarPath, '/resources/grammar-csv.txt');
	assert.equal(list[0].source, 'emit(sum(col("amount")));');
});

test('POST /api/queries rejects a missing field', async () => {
	const res = await fetch(`${base}/api/queries`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json', 'x-user-id': 'alice@test' },
		body: JSON.stringify({ name: 'incomplete' }), // no grammarPath/source
	});
	assert.equal(res.status, 400);
});

test('queries are isolated per user - a different x-user-id sees none of the above', async () => {
	const res = await fetch(`${base}/api/queries`, { headers: { 'x-user-id': 'bob@test' } });
	const list = await res.json();
	assert.deepEqual(list, []);
});

test('POST then GET /api/dashboards round-trips layout as a real object, not a string', async () => {
	const headers = { 'Content-Type': 'application/json', 'x-user-id': 'alice@test' };
	const layout = { tiles: [{ query: 'big spenders', chart: 'bar' }] };

	const postRes = await fetch(`${base}/api/dashboards`, {
		method: 'POST',
		headers,
		body: JSON.stringify({ name: 'spend overview', layout }),
	});
	assert.equal(postRes.status, 201);

	const getRes = await fetch(`${base}/api/dashboards`, { headers });
	const list = await getRes.json();
	assert.equal(list.length, 1);
	assert.equal(list[0].name, 'spend overview');
	assert.deepEqual(list[0].layout, layout);
});

test('POST /api/dashboards rejects a non-object layout', async () => {
	const res = await fetch(`${base}/api/dashboards`, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json', 'x-user-id': 'alice@test' },
		body: JSON.stringify({ name: 'bad', layout: 'not an object' }),
	});
	assert.equal(res.status, 400);
});

test('static host serves index.html', async () => {
	const res = await fetch(`${base}/`);
	assert.equal(res.status, 200);
	const text = await res.text();
	assert.match(text, /web-csv/);
});
