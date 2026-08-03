import { Router } from 'express';
import { getDb } from '../db.js';
import { requireUser } from './auth.js';

export const queriesRouter = Router();

queriesRouter.use(requireUser);

queriesRouter.get('/', (req, res) => {
	const rows = getDb()
		.prepare(
			'SELECT id, name, grammar_path AS grammarPath, source, created_at AS createdAt ' +
			'FROM queries WHERE owner_id = ? ORDER BY created_at DESC',
		)
		.all(req.user.id);
	res.json(rows);
});

queriesRouter.post('/', (req, res) => {
	const { name, grammarPath, source } = req.body ?? {};
	if (!name || !grammarPath || typeof source !== 'string') {
		res.status(400).json({ error: 'name, grammarPath, and source are required' });
		return;
	}

	const info = getDb()
		.prepare('INSERT INTO queries (owner_id, name, grammar_path, source) VALUES (?, ?, ?, ?)')
		.run(req.user.id, name, grammarPath, source);

	res.status(201).json({ id: info.lastInsertRowid });
});
