// app.js - the configured Express app: persistence API + static host for
// web/. Split out from index.js (which just calls .listen()) so tests can
// import the app and drive it directly, on an ephemeral port, against a
// throwaway database - see ../test/api.test.js.

import express from 'express';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { queriesRouter } from './routes/queries.js';
import { dashboardsRouter } from './routes/dashboards.js';

const __dirname = dirname(fileURLToPath(import.meta.url));

export const app = express();

app.use(express.json());
app.use('/api/queries', queriesRouter);
app.use('/api/dashboards', dashboardsRouter);
app.use(express.static(join(__dirname, '..', '..', 'web')));
