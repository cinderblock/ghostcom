// Diagnostic 3: try to open COM port from same process
import { createRequire } from 'node:module';
import { openSync, readSync, writeSync, closeSync } from 'node:fs';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');

// Clean up
for (const p of native.listPorts()) {
  try { native.destroyPort(p.companionIndex); } catch {}
}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Created COM${portNumber}, ci=${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();

const received = [];
stream.onData(chunk => {
  console.log(`[companion<-COM] ${chunk.length}b: "${chunk.toString()}"`);
  received.push(chunk);
});
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();

port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

// Try to open COM port from THIS process
const comPath = `\\\\.\\COM${portNumber}`;
console.log(`Opening ${comPath} from main process...`);

let comFd;
try {
  comFd = openSync(comPath, 'r+');
  console.log(`COM side fd=${comFd}`);
} catch (e) {
  console.error(`COM open failed: ${e.message}`);
  stream.shutdown(); port.shutdownSignals(); port.close();
  native.destroyPort(companionIndex);
  process.exit(1);
}

await sleep(200);

// Write from companion -> COM
console.log('\nTest 1: companion -> COM');
await new Promise((res, rej) =>
  stream.write(Buffer.from('PING\r\n'), e => e ? rej(e) : res())
);
console.log('Companion wrote PING');

await sleep(500);

const buf = Buffer.alloc(64);
let n = 0;
try {
  n = readSync(comFd, buf, 0, 64, null);
  console.log(`COM readSync got ${n} bytes: "${buf.slice(0, n).toString()}"`);
} catch (e) {
  console.error('COM readSync error:', e.message);
}

// Write from COM -> companion
console.log('\nTest 2: COM -> companion');
const written = writeSync(comFd, Buffer.from('PONG\r\n'));
console.log(`COM wrote ${written} bytes`);

await sleep(500);

const compStr = Buffer.concat(received).toString();
console.log('Companion received:', JSON.stringify(compStr));

closeSync(comFd);
stream.shutdown();
port.shutdownSignals();
port.close();
native.destroyPort(companionIndex);

console.log('\n=== RESULTS ===');
console.log('companion->COM:', n > 0 ? 'PASS' : 'FAIL');
console.log('COM->companion:', compStr.includes('PONG') ? 'PASS' : 'FAIL');
process.exit(n > 0 && compStr.includes('PONG') ? 0 : 1);
