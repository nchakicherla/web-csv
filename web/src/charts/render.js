// render.js - picks a form for a wcResults entry by its shape
// (choosing-a-form.md): a bare number is a stat tile, a groupby result is
// a bar chart (compare magnitude across categories) *unless* its labels
// are chronological (compare a trend over time instead - see
// looksChronological), in which case it's a line chart, and a whole
// column is a table (row-level data has no natural chart form of its
// own). Shared by the ad-hoc query results area and every dashboard tile
// - same result shapes, same rendering.

import { renderBarChart } from './bar.js';
import { renderLineChart } from './line.js';
import { renderStatTile } from './stat.js';
import { renderColumnTable } from './table.js';

// True when every label is a date_part()-shaped "YYYY"/"YYYY-MM"/
// "YYYY-MM-DD" string (see interp/ext/datetime.c's wcFormatDatePart for
// the exact formats) - deliberately excludes "weekday" labels ("Mon".."Sun"),
// which are cyclic/categorical, not a timeline. This is a shape check on
// the labels themselves, not a flag carried by the result: groupby()'s
// {type:'groups'} payload doesn't know whether date_part() produced its
// categorical input or a CSV column of literal "2024-01" strings did -
// and it shouldn't need to; either one is a real timeline and should
// render the same way. Needs at least 2 points, since a single date
// bucket has no trend to show as a line.
const DATE_GROUP_LABEL_RE = /^\d{4}(-\d{2}(-\d{2})?)?$/;
function looksChronological(labels) {
	return labels.length >= 2 && labels.every((l) => DATE_GROUP_LABEL_RE.test(l));
}

export function renderResult(container, result, label) {
	if (typeof result === 'number') {
		renderStatTile(container, { label: label || 'Result', value: result });
		return;
	}
	if (result && result.type === 'groups') {
		if (looksChronological(result.labels)) {
			renderLineChart(container, { title: label || `${result.agg} over time`, labels: result.labels, values: result.values });
		} else {
			renderBarChart(container, { title: label || `${result.agg} by category`, labels: result.labels, values: result.values });
		}
		return;
	}
	if (result && result.type === 'column') {
		renderColumnTable(container, { title: label || `Column (${result.dtype})`, dtype: result.dtype, values: result.values });
		return;
	}
	container.textContent = typeof result === 'string' ? result : JSON.stringify(result);
}

// Renders every result from a query run into `container`, one block per
// result, in order - a query with multiple emit()/groupby() calls
// produces multiple wcResults entries.
export function renderResults(container, results) {
	container.innerHTML = '';
	if (results.length === 0) {
		const empty = document.createElement('p');
		empty.textContent = 'No output - this query has no emit()/groupby() calls.';
		container.appendChild(empty);
		return;
	}
	results.forEach((result, i) => {
		const block = document.createElement('div');
		block.style.marginBottom = '8px';
		renderResult(block, result, results.length > 1 ? `Result ${i + 1}` : undefined);
		container.appendChild(block);
	});
}
