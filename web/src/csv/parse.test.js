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

test('detects a bare ISO date column and converts it to UTC epoch seconds', () => {
	const columns = parseCsv('order_date,amount\n2024-01-15,10\n2024-02-20,20\n');
	assert.equal(columns[0].type, 'date');
	// Hand-computed with Python's calendar.timegm, independent of this
	// codebase's own date math - see interp/ext/test/test_builtins.c's date
	// tests for the same values used the same way on the C side.
	assert.deepEqual(Array.from(columns[0].values), [1705276800, 1708387200]);
	assert.equal(columns[1].type, 'f64');
});

test('detects a date+time column (T-separated) as UTC, not local time', () => {
	const columns = parseCsv('ts\n2024-06-15T08:30:00\n');
	assert.equal(columns[0].type, 'date');
	assert.equal(columns[0].values[0], 1718440200);
});

test('a space-separated date+time is also detected, still as UTC', () => {
	const columns = parseCsv('ts\n2024-06-15 08:30\n');
	assert.equal(columns[0].type, 'date');
	assert.equal(columns[0].values[0], 1718440200); // same clock time as the T-separated case above, seconds default to 0
});

test('treats an empty cell in a date column as NaN, not a string column', () => {
	// A wholly-blank line would be dropped by parseCsv's line filter before
	// this even reaches column classification, so the empty date cell needs
	// a second, non-empty column on the same row to keep the row non-blank.
	const columns = parseCsv('d,x\n2024-01-15,1\n,2\n');
	assert.equal(columns[0].type, 'date');
	assert.equal(columns[0].values[0], 1705276800);
	assert.equal(Number.isNaN(columns[0].values[1]), true);
});

test('a column that merely looks date-shaped in one row but not another falls back to string', () => {
	const columns = parseCsv('mixed\n2024-01-15\nnot-a-date\n');
	assert.equal(columns[0].type, 'string');
	assert.deepEqual(columns[0].values, ['2024-01-15', 'not-a-date']);
});
