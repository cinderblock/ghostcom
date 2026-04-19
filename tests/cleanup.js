import { createRequire } from 'node:module';
const req = createRequire(import.meta.url);
const n = req('../addon/ghostcom.node');
const ps = n.listPorts();
for (const p of ps) {
  try { n.destroyPort(p.companionIndex); console.log('destroyed', p.portNumber); }
  catch(e) { console.log('err', e.message); }
}
/*
 * The addon keeps worker threads / TSFNs alive after module load, so
 * bun's event loop won't exit naturally — which leaves a lingering
 * bun process holding \\.\GhostCOMControl and blocks the next test
 * file from opening the driver. Force exit so run-tests.ts can
 * proceed cleanly.
 */
process.exit(0);
