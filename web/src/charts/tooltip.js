// tooltip.js - the hover tooltip shared by every chart mark (bar.js's
// bars, line.js's points): one <div>, reused across chart instances
// rather than duplicated per chart, since only one tooltip can be visible
// on the page at a time anyway.

import { formatCompact } from './format.js';

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
// high-contrast element, the category/date label is secondary. Built with
// textContent/createTextNode, never innerHTML: labels are untrusted data
// (CSV headers/values), not markup this page authored.
export function showTooltip(pageX, pageY, label, value) {
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

export function hideTooltip() {
	if (tooltipEl) {
		tooltipEl.classList.remove('is-visible');
	}
}
