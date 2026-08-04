// line.js - a hand-rolled SVG line chart for a single-series trend over
// time: a groupby(date_part(...)) result whose labels are chronological
// "YYYY"/"YYYY-MM"/"YYYY-MM-DD" strings, not an arbitrary category name -
// see render.js's looksChronological() for the detection rule and
// interp/ext/datetime.c's wcFormatDatePart for the exact label formats
// this matches. Same chrome/margins/gridline conventions as bar.js so the
// two forms read as the same chart family - just a different mark for a
// different comparison (trend over time vs. magnitude across categories -
// choosing-a-form.md).

import { niceMax, formatTick, formatCompact } from './format.js';
import { buildTable } from './table.js';
import { showTooltip, hideTooltip } from './tooltip.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

function svgEl(tag, attrs) {
	const el = document.createElementNS(SVG_NS, tag);
	for (const [k, v] of Object.entries(attrs)) {
		el.setAttribute(k, String(v));
	}
	return el;
}

// Past a handful of points there's no room for every x-axis label without
// overlap - the same "measure first, don't let SVG silently overflow"
// rule bar.js's truncateLabel follows, but for point density rather than
// string length: show every Nth label instead of shortening each one.
function xLabelStride(n) {
	if (n <= 8) {
		return 1;
	}
	if (n <= 16) {
		return 2;
	}
	if (n <= 31) {
		return 4;
	}
	return Math.ceil(n / 8);
}

export function renderLineChart(container, { title, labels, values }) {
	container.innerHTML = '';

	const root = document.createElement('div');
	root.className = 'viz-root';

	if (title) {
		const titleEl = document.createElement('p');
		titleEl.className = 'viz-title';
		titleEl.textContent = title;
		root.appendChild(titleEl);
	}

	if (!labels.length) {
		const empty = document.createElement('p');
		empty.className = 'viz-table-note';
		empty.textContent = 'No data points to show.';
		root.appendChild(empty);
		container.appendChild(root);
		return;
	}

	const toolbar = document.createElement('div');
	toolbar.className = 'viz-toolbar';
	const toggleBtn = document.createElement('button');
	toggleBtn.type = 'button';
	toggleBtn.textContent = 'View as table';
	toolbar.appendChild(toggleBtn);
	root.appendChild(toolbar);

	const chartWrap = document.createElement('div');
	const tableWrap = document.createElement('div');
	tableWrap.hidden = true;

	const width = 480;
	const height = 220;
	const marginLeft = 44;
	const marginBottom = 30;
	const marginTop = 12;
	const marginRight = 12;
	const plotW = width - marginLeft - marginRight;
	const plotH = height - marginTop - marginBottom;

	const n = values.length;
	// 0-based axis, the same convention bar.js uses - and the same known
	// limitation: a series with a negative value (a net-refund month, say)
	// would sit below a baseline this scale doesn't extend to accommodate.
	// Not handled here any more than it is in bar.js - see README's "Known
	// gaps".
	const maxVal = Math.max(0, ...values);
	const axisMax = niceMax(maxVal || 1);

	const svg = svgEl('svg', {
		viewBox: `0 0 ${width} ${height}`,
		width: '100%',
		height,
		role: 'img',
		'aria-label': title || 'Line chart',
	});

	// Gridlines at 0 / half / max - clean numbers, per marks-and-anatomy.md.
	for (const t of [0, axisMax / 2, axisMax]) {
		const y = marginTop + plotH - (t / axisMax) * plotH;
		svg.appendChild(svgEl('line', {
			class: t === 0 ? 'viz-axis-line' : 'viz-gridline',
			x1: marginLeft, x2: marginLeft + plotW, y1: y, y2: y,
		}));
		const tickLabel = svgEl('text', { class: 'viz-tick-label', x: marginLeft - 6, y: y + 3, 'text-anchor': 'end' });
		tickLabel.textContent = formatTick(t);
		svg.appendChild(tickLabel);
	}

	// n===1 has no "between" to draw a line across - still plots the one
	// point (centered), just no <path>.
	const xAt = (i) => (n === 1 ? marginLeft + plotW / 2 : marginLeft + (i / (n - 1)) * plotW);
	const yAt = (v) => marginTop + plotH - (axisMax > 0 ? (v / axisMax) * plotH : 0);

	if (n > 1) {
		const d = values.map((v, i) => `${i === 0 ? 'M' : 'L'} ${xAt(i)} ${yAt(v)}`).join(' ');
		svg.appendChild(svgEl('path', { class: 'viz-line', d, fill: 'none' }));
	}

	const stride = xLabelStride(n);
	values.forEach((v, i) => {
		const x = xAt(i);
		const y = yAt(v);
		const label = labels[i] ?? '';

		// Hit target bigger than the mark (interaction.md): a generous
		// invisible circle around each point, not just the 3px dot itself.
		const hit = svgEl('circle', { class: 'viz-line-hit', cx: x, cy: y, r: 10, tabindex: '0' });
		const point = svgEl('circle', { class: 'viz-line-point', cx: x, cy: y, r: 3 });

		const onShow = (evt) => showTooltip(evt.pageX ?? 0, evt.pageY ?? 0, label, v);
		hit.addEventListener('pointerenter', onShow);
		hit.addEventListener('pointermove', onShow);
		hit.addEventListener('pointerleave', hideTooltip);
		hit.addEventListener('focus', () => {
			const rect = hit.getBoundingClientRect();
			showTooltip(rect.left + window.scrollX + rect.width / 2, rect.top + window.scrollY, label, v);
		});
		hit.addEventListener('blur', hideTooltip);

		svg.appendChild(hit);
		svg.appendChild(point);

		if (i % stride === 0 || i === n - 1) {
			const xLabel = svgEl('text', { class: 'viz-category-label', x, y: marginTop + plotH + 14, 'text-anchor': 'middle' });
			xLabel.textContent = label;
			svg.appendChild(xLabel);
		}
	});

	chartWrap.appendChild(svg);
	tableWrap.appendChild(buildTable(['Date', 'Value'], labels.map((l, i) => [l, formatCompact(values[i])])));

	toggleBtn.addEventListener('click', () => {
		const switchingToTable = tableWrap.hidden;
		tableWrap.hidden = !switchingToTable;
		chartWrap.hidden = switchingToTable;
		toggleBtn.textContent = switchingToTable ? 'View as chart' : 'View as table';
	});

	root.appendChild(chartWrap);
	root.appendChild(tableWrap);
	container.appendChild(root);
}
