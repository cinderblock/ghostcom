import { createRequire } from 'node:module';
const r = createRequire(import.meta.url);
const n = r('../addon/ghostcom.node');
for (const num of [20, 21, 22, 23]) {
  try {
    const p = n.createPort(num);
    console.log(`COM${num} created: ci=${p.companionIndex}`);
    n.destroyPort(p.companionIndex);
    console.log(`COM${num} destroyed ok`);
    process.exit(0);
  } catch(e) {
    console.log(`COM${num} failed: ${e.message}`);
  }
}
process.exit(1);
