// table.js - the plain-HTML table every chart needs as its accessibility
// twin (components.md's "table-view generator"), and the primary view for
// a raw emit(column) result, which has no natural chart form of its own
// (it's row-level data, not a magnitude-per-category or trend-over-time
// shape - see choosing-a-form.md).

const MAX_ROWS = 200;

// Cell values go in via textContent, never innerHTML - column values
// originate from user-uploaded CSV data, not markup this page authored
// (interaction.md's "labels are untrusted data" rule applies to table
// cells the same as tooltips).
export function buildTable(headers, rows) {
	const table = document.createElement('table');
	table.className = 'viz-table';

	const thead = document.createElement('thead');
	const headRow = document.createElement('tr');
	for (const h of headers) {
		const th = document.createElement('th');
		th.textContent = h;
		headRow.appendChild(th);
	}
	thead.appendChild(headRow);
	table.appendChild(thead);

	const tbody = document.createElement('tbody');
	const shown = rows.slice(0, MAX_ROWS);
	for (const row of shown) {
		const tr = document.createElement('tr');
		for (const cell of row) {
			const td = document.createElement('td');
			td.textContent = cell;
			tr.appendChild(td);
		}
		tbody.appendChild(tr);
	}
	table.appendChild(tbody);

	const wrap = document.createElement('div');
	wrap.appendChild(table);
	if (rows.length > MAX_ROWS) {
		const note = document.createElement('p');
		note.className = 'viz-table-note';
		note.textContent = `Showing first ${MAX_ROWS} of ${rows.length} rows.`;
		wrap.appendChild(note);
	}
	return wrap;
}

export function renderColumnTable(container, { title, dtype, values }) {
	container.innerHTML = '';
	const root = document.createElement('div');
	root.className = 'viz-root';

	if (title) {
		const titleEl = document.createElement('p');
		titleEl.className = 'viz-title';
		titleEl.textContent = title;
		root.appendChild(titleEl);
	}

	root.appendChild(buildTable(
		['#', dtype === 'f64' ? 'Value' : 'Category'],
		values.map((v, i) => [i + 1, v]),
	));

	container.appendChild(root);
}
