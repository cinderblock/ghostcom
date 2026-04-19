// Diagnostic 8: Run Python ReadFileEx test alongside companion write.
// If Python's APC fires → driver is correct, issue is Bun.
// If Python's APC doesn't fire → driver bug.
import { createRequire } from 'node:module';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

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

// Start Python ReadFileEx test
const py = spawn('C:\\Users\\test\\AppData\\Local\\Microsoft\\WindowsApps\\python.exe', [
  'tests/test-readfileex.py', `COM${portNumber}`
], { cwd: 'C:\\GhostCOM-src' });

let pyOut = '';
py.stdout.on('data', d => { pyOut += d.toString(); process.stdout.write('[PY] ' + d.toString()); });
py.stderr.on('data', d => process.stderr.write('[PY ERR] ' + d.toString()));

// Wait for Python to open the port and issue ReadFileEx
console.log('Waiting for Python to open port and issue ReadFileEx...');
const deadline = Date.now() + 5000;
while (Date.now() < deadline && !pyOut.includes('Entering SleepEx')) {
  await sleep(50);
}

if (!pyOut.includes('SleepEx')) {
  console.error('Python did not reach SleepEx!');
  py.kill();
  stream.shutdown(); port.shutdownSignals(); port.close();
  native.destroyPort(companionIndex);
  process.exit(1);
}

console.log('\nPython in SleepEx. Writing from companion...');
await sleep(200);
await new Promise((res, rej) => stream.write(Buffer.from('PING\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote PING. Waiting for Python APC...');

// Wait for Python to finish (8s timeout)
await new Promise(res => {
  const t = setTimeout(() => { py.kill(); res(); }, 10000);
  py.on('close', () => { clearTimeout(t); res(); });
});

console.log('\n=== DIAGNOSIS ===');
if (pyOut.includes('APC fired')) {
  console.log('✓ Python received data via APC — driver is CORRECT');
  console.log('  → The serialport issue is Bun-specific (uv_async_send from CreateThread)');
} else if (pyOut.includes('TIMEOUT')) {
  console.log('✗ Python did NOT receive data — DRIVER BUG in ReadFileEx APC path');
  console.log('  → GcomDrainRingToReads is not correctly queuing APC completions');
} else {
  console.log('? Unexpected output:', pyOut.slice(-200));
}

stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
