import { createRequire } from 'node:module';
const req = createRequire(import.meta.url);
const n = req('../addon/ghostcom.node');
const ps = n.listPorts();
for (const p of ps) {
  try { n.destroyPort(p.companionIndex); console.log('destroyed', p.portNumber); }
  catch(e) { console.log('err', e.message); }
}
