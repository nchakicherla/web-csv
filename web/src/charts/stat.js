// stat.js - the stat-tile contract from marks-and-anatomy.md: label +
// value (auto-compact). No delta/trend - those need a prior period to
// compare against, which nothing in this project tracks yet, and the
// contract lists both as optional. A bare stat tile skips the hover
// layer entirely (interaction.md) - there's no mark to hover, just a
// number.

import { formatCompact } from './format.js';

export function renderStatTile(container, { label, value }) {
	container.innerHTML = '';
	const root = document.createElement('div');
	root.className = 'viz-root';

	const labelEl = document.createElement('p');
	labelEl.className = 'viz-stat-label';
	labelEl.textContent = label;
	root.appendChild(labelEl);

	const valueEl = document.createElement('p');
	valueEl.className = 'viz-stat-value';
	valueEl.textContent = formatCompact(value);
	root.appendChild(valueEl);

	container.appendChild(root);
}
