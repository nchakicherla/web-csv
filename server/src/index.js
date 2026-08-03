// index.js - persistence API + static host for web/.
//
// The compute path (CSV parsing, the WASM interpreter, WebGPU) is entirely
// client-side and works with this process never running - see
// ../../docs/ARCHITECTURE.md's deployment-model note. This server exists
// only for the parts that need somewhere durable to live: saved queries,
// saved dashboards.

import express from 'express';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { queriesRouter } from './routes/queries.js';
import { dashboardsRouter } from './routes/dashboards.js';

const __dirname = dirname(fileURLToPath(import.meta.url));
const app = express();

app.use(express.json());
app.use('/api/queries', queriesRouter);
app.use('/api/dashboards', dashboardsRouter);
app.use(express.static(join(__dirname, '..', '..', 'web')));

const port = process.env.PORT || 8787;
app.listen(port, () => {
	console.log(`web-csv listening on http://localhost:${port}`);
});
