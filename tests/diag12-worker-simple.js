import path from 'node:path';
import { setTimeout as sleep } from 'node:timers/promises';

const workerUrl = path.resolve(import.meta.dir, './worker-simple.js');
const worker = new Worker(workerUrl);

const messages = [];
const done = new Promise(res => {
  worker.onmessage = e => {
    console.log('[worker]', JSON.stringify(e.data));
    messages.push(e.data);
    if (e.data.type === 'done') res();
  };
  worker.onerror = e => { console.error('[error]', e.message); res(); };
});

await Promise.race([done, sleep(5000)]);
worker.terminate();

console.log('Messages received:', messages.length);
console.log('bun:ffi in Worker:', messages.some(m => m.type === 'ffi_ok') ? 'SUPPORTED' : 'NOT supported');
