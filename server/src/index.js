// index.js - starts the server (see app.js for what it actually serves).
//
// The compute path (CSV parsing, the WASM interpreter, WebGPU) is entirely
// client-side and works with this process never running - see
// ../../docs/ARCHITECTURE.md's deployment-model note. This server exists
// only for the parts that need somewhere durable to live: saved queries,
// saved dashboards.

import { app } from './app.js';

const port = process.env.PORT || 8787;
app.listen(port, () => {
	console.log(`web-csv listening on http://localhost:${port}`);
});
