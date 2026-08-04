// parse.js - CSV text -> typed columns.
//
// A column is 'f64' (numeric), 'date' (ISO 8601 date, optionally with a
// time-of-day - see DATE_RE below), or 'string' (everything else -
// dictionary-encoded on the way into the interpreter's column store, see
// main.js's loadStringColumn / interp/ext/web_main.c's
// wc_load_column_str_dict / column.c's columnCreateStrDict).

// Bare date ("2024-01-15") or date+time joined with 'T' or a space
// ("2024-01-15T08:30:00", "2024-01-15 08:30"), seconds optional. Deliberately
// narrower than full ISO 8601 (no timezone offsets, no week-dates) - see
// parseIsoDateToEpochSeconds's comment for why offsets in particular are
// rejected rather than merely ignored.
const DATE_RE = /^\d{4}-\d{2}-\d{2}([T ]\d{2}:\d{2}(:\d{2})?)?$/;
const DATE_CAPTURE_RE = /^(\d{4})-(\d{2})-(\d{2})(?:[T ](\d{2}):(\d{2})(?::(\d{2}))?)?$/;

// Epoch seconds (UTC), matching interp/ext/datetime.c's wcParseDate on the
// C side - both need to agree on what a bare "YYYY-MM-DD[THH:MM[:SS]]"
// string means. Date.parse() is deliberately not used here: per the
// ECMA-262 spec, a date-*only* string parses as UTC but a date-*time*
// string with no explicit offset parses as the browser's *local* time zone
// - the same CSV would silently load different data depending on where
// it's opened. Building the epoch from Date.UTC(...) on the regex-captured
// fields sidesteps that entirely (and is also why DATE_RE has no timezone-
// offset alternative: this function has no UTC-conversion logic for one).
function parseIsoDateToEpochSeconds(v) {
	const m = DATE_CAPTURE_RE.exec(v);
	if (!m) {
		return NaN;
	}
	const [, y, mo, d, h, mi, s] = m;
	return Date.UTC(Number(y), Number(mo) - 1, Number(d), Number(h ?? 0), Number(mi ?? 0), Number(s ?? 0)) / 1000;
}

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

		// Checked after isNumeric (a numeric column always wins - DATE_RE
		// can't match a plain number anyway) and before falling back to
		// 'string'. Same empty-cell-as-NaN convention isNumeric uses above;
		// unlike a numeric NaN (which sum()/filter_gt() just treat as "not >
		// threshold"), a NaN date reaching wcFormatDatePart's floor()/cast
		// to int64 is undefined behavior in C - fine for now since this PoC
		// doesn't validate CSV data quality anywhere else either, but a
		// sharper edge than the numeric case if it's ever hit.
		const isDate = raw.length > 0 && raw.every((v) => v === '' || DATE_RE.test(v));
		if (isDate) {
			return {
				name,
				type: 'date',
				values: Float64Array.from(raw.map((v) => (v === '' ? NaN : parseIsoDateToEpochSeconds(v)))),
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
