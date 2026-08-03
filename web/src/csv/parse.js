// parse.js - CSV text -> typed columns.
//
// A column is either 'f64' (numeric) or 'string' (everything else -
// dictionary-encoded on the way into the interpreter's column store, see
// main.js's loadStringColumn / interp/ext/web_main.c's
// wc_load_column_str_dict / column.c's columnCreateStrDict).

export function parseCsv(text) {
	const lines = text.split(/\r\n|\n/).filter((l) => l.length > 0);
	if (lines.length === 0) {
		return [];
	}

	const headers = splitCsvLine(lines[0]);
	const rows = lines.slice(1).map(splitCsvLine);

	return headers.map((name, colIdx) => {
		const raw = rows.map((r) => (r[colIdx] ?? '').trim());
		const isNumeric = raw.length > 0 && raw.every((v) => v === '' || Number.isFinite(Number(v)));

		if (isNumeric) {
			return {
				name,
				type: 'f64',
				values: Float64Array.from(raw.map((v) => (v === '' ? NaN : Number(v)))),
			};
		}
		return { name, type: 'string', values: raw };
	});
}

// Minimal CSV field split: handles quoted fields with embedded commas, not
// escaped ("") quotes or embedded newlines inside a field. Enough to
// ingest a plain CSV for this PoC; swap in a real RFC 4180 parser before
// trusting this on arbitrary user-uploaded files.
function splitCsvLine(line) {
	const out = [];
	let cur = '';
	let inQuotes = false;

	for (let i = 0; i < line.length; i++) {
		const c = line[i];
		if (inQuotes) {
			if (c === '"') {
				inQuotes = false;
			} else {
				cur += c;
			}
		} else if (c === '"') {
			inQuotes = true;
		} else if (c === ',') {
			out.push(cur);
			cur = '';
		} else {
			cur += c;
		}
	}
	out.push(cur);
	return out;
}
