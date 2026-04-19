// Quick diagnostic script to identify where data transfer breaks
import { createRequire } from 'node:module';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');
const { SerialPort } = await import('serialport');

console.log('=== DIAGNOSTIC ===');

// List existing ports and clean up
for (const p of native.listPorts()) {
  if (p.portNumber === 20) {
    try { native.destroyPort(p.companionIndex); } catch {}
  }
}
await sleep(200);

// Create port
console.log('1. Creating COM20...');
const { portNumber, companionIndex } = native.createPort(20);
console.log(`   portNumber=${portNumber} companionIndex=${companionIndex}`);

// Open companion
console.log('2. Opening companion GCOM' + companionIndex + '...');
const port = native.openPort(companionIndex);
const stream = port.createStream();

const companionChunks = [];
stream.onData(chunk => {
  console.log(`   [companion<-COM] received ${chunk?.length ?? 0} bytes:`, chunk?.toString?.() ?? '(null)');
  if (chunk) companionChunks.push(chunk);
});
stream.onReadError(err => console.error('   [companion] read error:', err));
stream.resumeReading();

port.onSignalChange(raw => {
  console.log(`   [signal] changedMask=0x${raw.changedMask.toString(16)} baud=${raw.baudRate}`);
});

await sleep(100); // let signal watcher start

// Open COM side
console.log('3. Opening COM20 with serialport...');
const serial = new SerialPort({ path: 'COM20', baudRate: 9600, autoOpen: false });
await new Promise((res, rej) => serial.open(e => e ? rej(e) : res()));
console.log('   COM20 opened!');

const comChunks = [];
serial.on('data', chunk => {
  console.log(`   [COM<-companion] received ${chunk.length} bytes:`, chunk.toString());
  comChunks.push(chunk);
});

await sleep(500); // let serialport set up reader

// Test direction 1: companion → COM
console.log('');
console.log('4. Writing "PING" from companion to COM...');
await new Promise((res, rej) =>
  stream.write(Buffer.from('PING\r\n'), e => e ? rej(e) : res())
);
console.log('   Write callback fired (companion→COM write returned)');

await sleep(2000);
console.log(`   COM received ${comChunks.reduce((n,c) => n+c.length, 0)} bytes total`);

// Test direction 2: COM → companion
console.log('');
console.log('5. Writing "PONG" from COM to companion...');
await new Promise((res, rej) => serial.write('PONG\r\n', e => e ? rej(e) : res()));
await new Promise(res => serial.drain(res));
console.log('   serial.drain() completed (COM→companion write returned)');

await sleep(2000);
console.log(`   Companion received ${companionChunks.reduce((n,c) => n+c.length, 0)} bytes total`);

// Report
console.log('');
console.log('=== RESULTS ===');
console.log('companion→COM:', comChunks.length > 0 ? 'PASS' : 'FAIL');
console.log('COM→companion:', companionChunks.length > 0 ? 'PASS' : 'FAIL');

// Cleanup
await new Promise(res => serial.close(res));
stream.shutdown();
port.shutdownSignals();
port.close();
native.destroyPort(companionIndex);
process.exit(0);
