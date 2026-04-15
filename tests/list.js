import { createRequire } from 'node:module';
const req = createRequire(import.meta.url);
const n = req('../addon/ghostcom.node');
console.log('ports:', JSON.stringify(n.listPorts()));
console.log('driver:', n.isDriverAvailable());
