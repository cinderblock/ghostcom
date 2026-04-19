// Diagnostic 2: use PowerShell .NET SerialPort (bypasses serialport npm package)
// to test if the driver's companion→COM data path works at all.
import { createRequire } from 'node:module';
import { spawn } from 'node:child_process';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');

console.log('=== DIAGNOSTIC 2: PS SerialPort ===');

// Clean up
for (const p of native.listPorts()) {
  if (p.portNumber === 20) {
    try { native.destroyPort(p.companionIndex); } catch {}
  }
}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0); // auto-assign
console.log(`Port: COM${portNumber}, companion index: ${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();
stream.onData(chunk => {
  console.log(`[companion<-COM] ${chunk.length} bytes:`, chunk.toString());
});
stream.onReadError(err => console.error('[companion read error]', err));
stream.resumeReading();

port.onSignalChange(s => {
  console.log(`[signal] 0x${s.changedMask.toString(16)}`);
});
await sleep(100);

// Start PowerShell reader in background (it opens COM20 and reads)
console.log('\nStarting PowerShell COM reader...');
const ps = spawn('C:\\Users\\test\\.bun\\bin\\bun.exe', [
  'run', 'tests/com-reader.js', `COM${portNumber}`, '6', '8000'
], { cwd: 'C:\\GhostCOM-src' });

let psOutput = '';
ps.stdout.on('data', d => { psOutput += d; process.stdout.write('[PS] ' + d); });
ps.stderr.on('data', d => process.stderr.write('[PS ERR] ' + d));

// Wait for PS to signal it's open
let psOpen = false;
const deadline = Date.now() + 5000;
while (!psOpen && Date.now() < deadline) {
  if (psOutput.includes('OPEN')) { psOpen = true; break; }
  await sleep(50);
}

if (!psOpen) {
  console.error('PowerShell failed to open COM port');
  ps.kill();
} else {
  console.log('PowerShell opened COM20. Waiting 500ms then writing...');
  await sleep(500);

  console.log('\nWriting "PING\\r\\n" from companion...');
  await new Promise((res, rej) =>
    stream.write(Buffer.from('PING\r\n'), e => e ? rej(e) : res())
  );
  console.log('Companion write callback fired.');
}

// Wait for PS to finish
await new Promise(res => ps.on('close', res));

console.log('\n=== RESULT ===');
const received = psOutput.includes('RECEIVED:6');
console.log('companion→COM via PS SerialPort:', received ? 'PASS' : 'FAIL');
console.log('Full PS output:', psOutput.trim());

// Cleanup
stream.shutdown();
port.shutdownSignals();
port.close();
native.destroyPort(companionIndex);
process.exit(received ? 0 : 1);
