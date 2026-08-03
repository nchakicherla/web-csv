// bar.js - a hand-rolled SVG bar chart for a single-series magnitude
// comparison (one aggregate value per category - a groupby() result).
// Sequential single-hue color per the dataviz skill's choosing-a-form.md
// ("compare magnitude" -> sequential, one hue), not per-category color -
// there's no series identity here to encode, just one measure per bar.
//
// No chart library: this project has no build step for web/ (plain ES
// modules), so charts are plain SVG + DOM, styled entirely through
// theme.css's CSS custom properties (see that file for the palette
// source - the dataviz skill's reference palette).

import { niceMax, formatTick, formatCompact } from './format.js';
import { buildTable } from './table.js';

const SVG_NS = 'http://www.w3.org/2000/svg';

let tooltipEl = null;

function getTooltip() {
	if (!tooltipEl) {
		tooltipEl = document.createElement('div');
		tooltipEl.className = 'viz-tooltip';
		document.body.appendChild(tooltipEl);
	}
	return tooltipEl;
}

// Values lead, labels follow (interaction.md) - the number is the
// high-contrast element, the category name is secondary. Built with
// textContent/createTextNode, never innerHTML: category labels are
// untrusted data (CSV headers/values), not markup this page authored.
function showTooltip(pageX, pageY, label, value) {
	const el = getTooltip();
	el.textContent = '';
	const valueEl = document.createElement('span');
	valueEl.className = 'viz-tooltip-value';
	valueEl.textContent = formatCompact(value);
	el.appendChild(valueEl);
	el.appendChild(document.createTextNode(` ${label}`));
	el.style.left = `${pageX}px`;
	el.style.top = `${pageY - 12}px`;
	el.classList.add('is-visible');
}

function hideTooltip() {
	if (tooltipEl) {
		tooltipEl.classList.remove('is-visible');
	}
}

// A bar with rounded top corners, square at the baseline (marks-and-
// anatomy.md's data-end spec) - a plain <rect> can't do one-sided
// rounding, hence a path.
function roundedTopBarPath(x, yTop, w, h, r) {
	const yBottom = yTop + h;
	if (h <= 0) {
		return `M ${x} ${yBottom} L ${x + w} ${yBottom} Z`;
	}
	r = Math.min(r, w / 2, h);
	return [
		`M ${x} ${yBottom}`,
		`L ${x} ${yTop + r}`,
		`Q ${x} ${yTop} ${x + r} ${yTop}`,
		`L ${x + w - r} ${yTop}`,
		`Q ${x + w} ${yTop} ${x + w} ${yTop + r}`,
		`L ${x + w} ${yBottom}`,
		'Z',
	].join(' ');
}

function svgEl(tag, attrs) {
	const el = document.createElementNS(SVG_NS, tag);
	for (const [k, v] of Object.entries(attrs)) {
		el.setAttribute(k, String(v));
	}
	return el;
}

// A label that won't fit doesn't get clipped (marks-and-anatomy.md) -
// this is the "measure first" call for the category axis: past a handful
// of bars there's no room for full names, so shorten rather than let SVG
// silently overflow into neighbors. Full names stay reachable in the
// table-view twin and the tooltip either way.
function truncateLabel(label, barCount) {
	const budget = barCount <= 6 ? 12 : barCount <= 12 ? 8 : 5;
	return label.length > budget ? `${label.slice(0, budget - 1)}…` : label;
}

export function renderBarChart(container, { title, labels, values }) {
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
		empty.textContent = 'No groups to show.';
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

	const maxVal = Math.max(0, ...values);
	const axisMax = niceMax(maxVal || 1);
	const n = values.length;
	const slot = plotW / n;
	const barGap = 2;
	const barWidth = Math.max(1, Math.min(24, slot - barGap));
	const showDirectLabels = n <= 12;

	const svg = svgEl('svg', {
		viewBox: `0 0 ${width} ${height}`,
		width: '100%',
		height,
		role: 'img',
		'aria-label': title || 'Bar chart',
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

	values.forEach((v, i) => {
		const barH = axisMax > 0 ? (v / axisMax) * plotH : 0;
		const x = marginLeft + i * slot + (slot - barWidth) / 2;
		const yTop = marginTop + plotH - barH;
		const label = labels[i] ?? '';

		// Hit target bigger than the mark (interaction.md): the full
		// column slot, not just the painted bar width.
		const hit = svgEl('rect', {
			class: 'viz-bar-hit',
			x: marginLeft + i * slot, y: marginTop, width: slot, height: plotH,
			tabindex: '0',
		});
		const bar = svgEl('path', { class: 'viz-bar', d: roundedTopBarPath(x, yTop, barWidth, barH, 4) });

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
		svg.appendChild(bar);

		if (showDirectLabels) {
			const valLabel = svgEl('text', { class: 'viz-value-label', x: x + barWidth / 2, y: yTop - 4, 'text-anchor': 'middle' });
			valLabel.textContent = formatCompact(v);
			svg.appendChild(valLabel);
		}

		const catLabel = svgEl('text', { class: 'viz-category-label', x: x + barWidth / 2, y: marginTop + plotH + 14, 'text-anchor': 'middle' });
		catLabel.textContent = truncateLabel(label, n);
		svg.appendChild(catLabel);
	});

	chartWrap.appendChild(svg);
	tableWrap.appendChild(buildTable(['Category', 'Value'], labels.map((l, i) => [l, formatCompact(values[i])])));

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
