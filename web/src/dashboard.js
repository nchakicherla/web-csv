// dashboard.js - a dashboard is a named list of tiles, each one query
// source string. Running a tile runs its query against whatever CSV is
// currently loaded and renders every result it emits (renderResults,
// charts/render.js). Saving/loading persists just {title, source} pairs
// through server/'s /api/dashboards (see api/client.js) - the results
// themselves are never persisted, only the queries that produce them, so
// a loaded dashboard always reflects whatever CSV is loaded when you run
// it, not a stale snapshot.

import { renderResults } from './charts/render.js';

let nextTileId = 1;

export function createDashboard({
	tilesContainer,
	addButton,
	runAllButton,
	saveButton,
	loadSelect,
	loadButton,
	getCurrentQuery,
	runQuery,
	saveDashboard,
	listDashboards,
	setStatus,
}) {
	let tiles = []; // {id, title, source, resultsEl}

	function renderTiles() {
		tilesContainer.innerHTML = '';
		if (tiles.length === 0) {
			const empty = document.createElement('p');
			empty.textContent = 'No tiles yet - write a query above and click "Add to dashboard".';
			tilesContainer.appendChild(empty);
			return;
		}

		for (const tile of tiles) {
			const card = document.createElement('div');
			card.className = 'tile-card';

			const header = document.createElement('div');
			header.className = 'tile-header';

			const title = document.createElement('strong');
			title.textContent = tile.title;
			header.appendChild(title);

			const runBtn = document.createElement('button');
			runBtn.type = 'button';
			runBtn.textContent = 'Run';
			runBtn.addEventListener('click', () => runTile(tile.id));
			header.appendChild(runBtn);

			const removeBtn = document.createElement('button');
			removeBtn.type = 'button';
			removeBtn.textContent = 'Remove';
			removeBtn.addEventListener('click', () => removeTile(tile.id));
			header.appendChild(removeBtn);

			card.appendChild(header);

			const source = document.createElement('pre');
			source.className = 'tile-source';
			source.textContent = tile.source;
			card.appendChild(source);

			const results = document.createElement('div');
			results.className = 'tile-results';
			card.appendChild(results);
			tile.resultsEl = results;

			tilesContainer.appendChild(card);
		}
	}

	function addTile(title, source) {
		tiles.push({ id: nextTileId++, title, source, resultsEl: null });
		renderTiles();
	}

	function removeTile(id) {
		tiles = tiles.filter((t) => t.id !== id);
		renderTiles();
	}

	async function runTile(id) {
		const tile = tiles.find((t) => t.id === id);
		if (!tile || !tile.resultsEl) {
			return;
		}
		tile.resultsEl.textContent = 'Running...';
		try {
			const results = await runQuery(tile.source);
			renderResults(tile.resultsEl, results);
		} catch (err) {
			tile.resultsEl.textContent = `Error: ${err.message}`;
		}
	}

	async function runAll() {
		setStatus('Running dashboard...');
		for (const tile of tiles) {
			await runTile(tile.id);
		}
		setStatus('Dashboard run complete.');
	}

	function toLayout() {
		return { tiles: tiles.map((t) => ({ title: t.title, source: t.source })) };
	}

	function loadFromLayout(layout) {
		tiles = (layout?.tiles ?? []).map((t) => ({ id: nextTileId++, title: t.title, source: t.source, resultsEl: null }));
		renderTiles();
	}

	async function refreshSavedList() {
		loadSelect.innerHTML = '';
		try {
			const saved = await listDashboards();
			if (saved.length === 0) {
				const opt = document.createElement('option');
				opt.textContent = '(no saved dashboards)';
				opt.disabled = true;
				loadSelect.appendChild(opt);
				return;
			}
			for (const d of saved) {
				const opt = document.createElement('option');
				opt.value = String(d.id);
				opt.textContent = d.name;
				opt.dataset.layout = JSON.stringify(d.layout);
				loadSelect.appendChild(opt);
			}
		} catch (err) {
			setStatus(`Failed to list dashboards: ${err.message}`);
		}
	}

	addButton.addEventListener('click', () => {
		const source = getCurrentQuery();
		if (!source.trim()) {
			setStatus('Query is empty - nothing to add.');
			return;
		}
		const title = window.prompt('Tile title:', `Tile ${tiles.length + 1}`);
		if (!title) {
			return;
		}
		addTile(title, source);
	});

	runAllButton.addEventListener('click', () => {
		runAll();
	});

	saveButton.addEventListener('click', async () => {
		if (tiles.length === 0) {
			setStatus('Add at least one tile before saving.');
			return;
		}
		const name = window.prompt('Save this dashboard as:');
		if (!name) {
			return;
		}
		try {
			await saveDashboard({ name, layout: toLayout() });
			setStatus(`Saved dashboard "${name}".`);
			await refreshSavedList();
		} catch (err) {
			setStatus(`Save failed: ${err.message}`);
		}
	});

	loadButton.addEventListener('click', () => {
		const opt = loadSelect.selectedOptions[0];
		if (!opt || !opt.dataset.layout) {
			return;
		}
		loadFromLayout(JSON.parse(opt.dataset.layout));
		setStatus(`Loaded dashboard "${opt.textContent}". Click "Run dashboard" to populate it.`);
	});

	renderTiles();
	refreshSavedList();

	return { addTile, removeTile, runTile, runAll, toLayout, loadFromLayout, refreshSavedList };
}
