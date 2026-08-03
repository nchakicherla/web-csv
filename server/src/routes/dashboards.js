import { Router } from 'express';
import { getDb } from '../db.js';
import { requireUser } from './auth.js';

export const dashboardsRouter = Router();

dashboardsRouter.use(requireUser);

dashboardsRouter.get('/', (req, res) => {
	const rows = getDb()
		.prepare(
			'SELECT id, name, layout, created_at AS createdAt ' +
			'FROM dashboards WHERE owner_id = ? ORDER BY created_at DESC',
		)
		.all(req.user.id);

	res.json(rows.map((r) => ({ ...r, layout: JSON.parse(r.layout) })));
});

dashboardsRouter.post('/', (req, res) => {
	const { name, layout } = req.body ?? {};
	if (!name || typeof layout !== 'object' || layout === null) {
		res.status(400).json({ error: 'name and layout (object) are required' });
		return;
	}

	const info = getDb()
		.prepare('INSERT INTO dashboards (owner_id, name, layout) VALUES (?, ?, ?)')
		.run(req.user.id, name, JSON.stringify(layout));

	res.status(201).json({ id: info.lastInsertRowid });
});
