import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseCsv } from './parse.js';

test('parses numeric columns as f64', () => {
	const columns = parseCsv('a,b\n1,2\n3,4\n');
	assert.equal(columns.length, 2);
	assert.equal(columns[0].type, 'f64');
	assert.deepEqual(Array.from(columns[0].values), [1, 3]);
	assert.equal(columns[1].type, 'f64');
	assert.deepEqual(Array.from(columns[1].values), [2, 4]);
});

test('detects a non-numeric column as string, leaves numeric columns alone', () => {
	const columns = parseCsv('name,amount\nalice,10\nbob,20\n');
	assert.equal(columns[0].name, 'name');
	assert.equal(columns[0].type, 'string');
	assert.deepEqual(columns[0].values, ['alice', 'bob']);
	assert.equal(columns[1].type, 'f64');
	assert.deepEqual(Array.from(columns[1].values), [10, 20]);
});

test('handles a quoted field containing a comma', () => {
	const columns = parseCsv('a,b\n"hello, world",5\n');
	assert.equal(columns[0].type, 'string');
	assert.deepEqual(columns[0].values, ['hello, world']);
	assert.equal(columns[1].type, 'f64');
	assert.deepEqual(Array.from(columns[1].values), [5]);
});

test('treats an empty cell in a numeric column as NaN, not a string column', () => {
	const columns = parseCsv('a,b\n1,\n2,5\n');
	assert.equal(columns[1].type, 'f64');
	assert.equal(Number.isNaN(columns[1].values[0]), true);
	assert.equal(columns[1].values[1], 5);
});

test('empty input returns no columns', () => {
	assert.deepEqual(parseCsv(''), []);
});

test('a header-only CSV (no data rows) classifies every column as string', () => {
	// Documents current behavior, not necessarily ideal: isNumeric requires
	// raw.length > 0, so with zero data rows there's nothing to infer a
	// numeric type from and every column falls back to 'string' with an
	// empty values array - worth knowing if a future change special-cases
	// this instead.
	const columns = parseCsv('a,b\n');
	assert.equal(columns[0].type, 'string');
	assert.deepEqual(columns[0].values, []);
});

test('trims whitespace around values', () => {
	const columns = parseCsv('a\n  42  \n');
	assert.equal(columns[0].type, 'f64');
	assert.equal(columns[0].values[0], 42);
});
