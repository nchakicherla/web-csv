// format.js - pure number-formatting helpers shared by the chart renderers.
// No DOM here on purpose, so these are plain-function testable (see
// format.test.js).

// Stat-tile contract (dataviz skill): auto-compact - 1,284 / 12.9K / 4.2M.
export function formatCompact(value) {
	if (!Number.isFinite(value)) {
		return String(value);
	}
	const abs = Math.abs(value);
	if (abs < 10000) {
		return value.toLocaleString(undefined, { maximumFractionDigits: 2 });
	}
	if (abs < 1e6) {
		return `${(value / 1e3).toFixed(1)}K`;
	}
	if (abs < 1e9) {
		return `${(value / 1e6).toFixed(1)}M`;
	}
	return `${(value / 1e9).toFixed(1)}B`;
}

// Rounds up to a "clean" axis maximum (1/2/5 * 10^n) so y-axis ticks land
// on round numbers rather than the raw data max.
export function niceMax(value) {
	if (!Number.isFinite(value) || value <= 0) {
		return 1;
	}
	const exp = Math.floor(Math.log10(value));
	const base = Math.pow(10, exp);
	const fraction = value / base;
	let niceFraction;
	if (fraction <= 1) {
		niceFraction = 1;
	} else if (fraction <= 2) {
		niceFraction = 2;
	} else if (fraction <= 5) {
		niceFraction = 5;
	} else {
		niceFraction = 10;
	}
	return niceFraction * base;
}

// Thousands-comma'd tick label - ticks are always "clean" numbers, never
// fractional, per marks-and-anatomy.md.
export function formatTick(value) {
	return Math.round(value).toLocaleString();
}
