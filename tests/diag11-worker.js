// Diagnostic 11: ReadFileEx APC on a Bun Worker (true separate thread)
import { createRequire } from 'node:module';
import { setTimeout as sleep } from 'node:timers/promises';
import path from 'node:path';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');
for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Port: COM${portNumber}, ci=${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();
stream.onData(() => {});
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();
port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

const workerUrl = path.resolve(import.meta.dir, './worker-com-reader.js');
const worker = new Worker(workerUrl);
// Send config immediately
worker.postMessage({ portNumber, comPath: `\\\\.\\COM${portNumber}` });

let apcFired = false;
let ready = false;

const workerDone = new Promise((resolve) => {
  worker.onmessage = (e) => {
    const msg = e.data;
    console.log(`[worker]`, msg);
    if (msg.type === 'ready') ready = true;
    if (msg.type === 'apc') apcFired = true;
    if (msg.type === 'done') resolve();
  };
  worker.onerror = (e) => {
    console.error('[worker error]', e.message);
    resolve();
  };
});

// Wait for worker to be ready
const deadline = Date.now() + 5000;
while (!ready && Date.now() < deadline) await sleep(50);

if (!ready) {
  console.error('Worker not ready!');
  worker.terminate();
} else {
  console.log('Worker ready. Writing from companion...');
  await sleep(200);
  await new Promise((res, rej) => stream.write(Buffer.from('BUNCTEST\r\n'), e => e ? rej(e) : res()));
  console.log('Companion wrote BUNCTEST');
  await Promise.race([workerDone, sleep(10000)]);
}

console.log('\n=== DIAGNOSIS ===');
if (apcFired) {
  console.log('✓ APC fired on Bun Worker thread — driver is CORRECT');
  console.log('  → serialport issue is uv_async_send() not working from');
  console.log('    serialport\'s CreateThread worker in Bun\'s event loop.');
  console.log('  → This is a Bun compatibility bug with native thread callbacks.');
} else {
  console.log('✗ APC did NOT fire even on Bun Worker thread');
  console.log('  → Either driver bug OR Bun Workers don\'t support true SleepEx APC delivery');
}

worker.terminate();
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
