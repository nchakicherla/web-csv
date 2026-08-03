// generate.mjs - makes the sample CSVs.
//
//   node generate.mjs <rows> <outfile>
//
// Deterministic on purpose: a fixed-seed PRNG (mulberry32), so the same
// row count always produces byte-identical output. That's what lets
// README quote exact expected query results - with Math.random() every
// regeneration would silently invalidate them.
//
// Rows are emitted in chronological order, which matters for more than
// tidiness: there's no date/time column type yet, so `month` is an
// ordinary categorical column and groupby returns its groups in
// dictionary (first-seen) order. Generating chronologically makes
// first-seen order and calendar order the same thing, so a
// groupby(col("month"), ...) chart reads Jan..Dec instead of scrambled.
// See ARCHITECTURE.md §5 for why the dictionary is first-seen ordered.

import { writeFileSync } from 'node:fs';

function mulberry32(seed) {
	return function () {
		seed |= 0;
		seed = (seed + 0x6d2b79f5) | 0;
		let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
		t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
		return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
	};
}

const MONTHS = ['jan', 'feb', 'mar', 'apr', 'may', 'jun', 'jul', 'aug', 'sep', 'oct', 'nov', 'dec'];
const DAYS_IN_MONTH = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
const REGIONS = ['north', 'south', 'east', 'west'];
const CHANNELS = ['online', 'in_store'];

// Category weights (relative), so groupby results aren't a flat line -
// a chart of six identical bars teaches nothing.
const CATEGORIES = [
	{ name: 'groceries', weight: 30, min: 8, max: 220 },
	{ name: 'coffee', weight: 22, min: 3, max: 14 },
	{ name: 'transport', weight: 14, min: 2, max: 95 },
	{ name: 'electronics', weight: 8, min: 40, max: 2400 },
	{ name: 'utilities', weight: 9, min: 60, max: 420 },
	{ name: 'dining', weight: 12, min: 12, max: 180 },
	{ name: 'travel', weight: 3, min: 180, max: 3800 },
	{ name: 'rent', weight: 2, min: 900, max: 2600 },
];

const TOTAL_WEIGHT = CATEGORIES.reduce((a, c) => a + c.weight, 0);

function pickCategory(rnd) {
	let r = rnd() * TOTAL_WEIGHT;
	for (const c of CATEGORIES) {
		r -= c.weight;
		if (r <= 0) return c;
	}
	return CATEGORIES[CATEGORIES.length - 1];
}

function generate(rows) {
	const rnd = mulberry32(20260803);
	const lines = ['id,date,month,category,region,channel,quantity,amount'];

	for (let i = 0; i < rows; i++) {
		// Chronological: walk the year proportionally to row index.
		const frac = i / rows;
		const monthIdx = Math.min(11, Math.floor(frac * 12));
		const day = 1 + Math.floor(rnd() * DAYS_IN_MONTH[monthIdx]);
		const date = `2025-${String(monthIdx + 1).padStart(2, '0')}-${String(day).padStart(2, '0')}`;

		const cat = pickCategory(rnd);
		const region = REGIONS[Math.floor(rnd() * REGIONS.length)];
		const channel = CHANNELS[Math.floor(rnd() * CHANNELS.length)];
		const quantity = 1 + Math.floor(rnd() * 5);
		const amount = (cat.min + rnd() * (cat.max - cat.min)).toFixed(2);

		lines.push(`${i + 1},${date},${MONTHS[monthIdx]},${cat.name},${region},${channel},${quantity},${amount}`);
	}
	return lines.join('\n') + '\n';
}

const rows = Number(process.argv[2] ?? 5000);
const out = process.argv[3] ?? `transactions-${rows}.csv`;
writeFileSync(out, generate(rows));
console.log(`wrote ${out} (${rows} rows)`);
