import { test } from 'node:test';
import assert from 'node:assert/strict';
import { formatCompact, niceMax, formatTick } from './format.js';

test('formatCompact: small numbers use locale formatting, not K/M', () => {
	assert.equal(formatCompact(0), '0');
	assert.equal(formatCompact(1284), '1,284');
	assert.equal(formatCompact(-42.5), '-42.5');
});

test('formatCompact: thousands compact to K', () => {
	assert.equal(formatCompact(12900), '12.9K');
	assert.equal(formatCompact(999999), '1000.0K');
});

test('formatCompact: millions compact to M', () => {
	assert.equal(formatCompact(4200000), '4.2M');
});

test('formatCompact: billions compact to B', () => {
	assert.equal(formatCompact(1500000000), '1.5B');
});

test('formatCompact: non-finite values pass through as strings', () => {
	assert.equal(formatCompact(NaN), 'NaN');
	assert.equal(formatCompact(Infinity), 'Infinity');
});

test('niceMax: rounds up to a clean 1/2/5 * 10^n ceiling', () => {
	assert.equal(niceMax(83), 100);
	assert.equal(niceMax(1.2), 2);
	assert.equal(niceMax(430), 500);
	assert.equal(niceMax(4328), 5000);
});

test('niceMax: handles non-positive input without throwing', () => {
	assert.equal(niceMax(0), 1);
	assert.equal(niceMax(-5), 1);
	assert.equal(niceMax(NaN), 1);
});

test('formatTick: rounds and comma-formats, never fractional', () => {
	assert.equal(formatTick(2500), '2,500');
	assert.equal(formatTick(2499.6), '2,500');
	assert.equal(formatTick(0), '0');
});
