// Diagnostic 7: full bidirectional test using FFI (bypasses serialport)
// Both directions tested: companion<->COM
import { createRequire } from 'node:module';
import { dlopen, FFIType } from 'bun:ffi';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');
for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Port: COM${portNumber}, ci=${companionIndex}`);

// --- Companion side ---
const port = native.openPort(companionIndex);
const stream = port.createStream();
const compReceived = [];
stream.onData(c => { compReceived.push(c); console.log(`[comp<-COM] ${c.length}b: "${c.toString().trim()}"`); });
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();
port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

// --- COM side via FFI ---
const { symbols: { CreateFileW, CloseHandle, CreateEventW, ReadFile, WriteFile, WaitForSingleObject, GetOverlappedResult, GetLastError, ResetEvent } } = dlopen('kernel32.dll', {
  CreateFileW: { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle: { args: ['ptr'], returns: FFIType.bool },
  CreateEventW: { args: ['ptr','bool','bool','ptr'], returns: FFIType.pointer },
  ReadFile: { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  WriteFile: { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  WaitForSingleObject: { args: ['ptr','u32'], returns: FFIType.u32 },
  GetOverlappedResult: { args: ['ptr','ptr','ptr','bool'], returns: FFIType.bool },
  GetLastError: { args: [], returns: FFIType.u32 },
  ResetEvent: { args: ['ptr'], returns: FFIType.bool },
});

const enc16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };
const hCom = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);
console.log(`COM opened, err=${GetLastError()}`);

const mkOv = (hEvt) => {
  const ov = Buffer.alloc(32);
  const p = BigInt(hEvt.valueOf ? hEvt.valueOf() : hEvt);
  ov.writeBigUInt64LE(p, 24); // hEvent at offset 24
  return ov;
};

const hEvt1 = CreateEventW(null, true, false, null);
const hEvt2 = CreateEventW(null, true, false, null);

// ── Test 1: companion → COM ──────────────────────────────────────────────

console.log('\n=== TEST 1: companion → COM ===');

// Issue overlapped ReadFile on COM BEFORE companion writes
const ov1 = mkOv(hEvt1);
const rbuf1 = Buffer.alloc(64);
const readOk1 = ReadFile(hCom, rbuf1, 64, null, ov1);
console.log(`ReadFile(pending): ok=${readOk1}, err=${GetLastError()}`);

await sleep(200);

// Now companion writes
await new Promise((res, rej) => stream.write(Buffer.from('Hello COM!\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote "Hello COM!"');

// Wait for read to complete
const w1 = WaitForSingleObject(hEvt1, 3000);
if (w1 === 0) {
  const n1b = Buffer.alloc(4);
  GetOverlappedResult(hCom, ov1, n1b, false);
  const n1 = n1b.readUInt32LE(0);
  console.log(`COM received ${n1} bytes: "${rbuf1.slice(0,n1).toString().trim()}"`);
  console.log('TEST 1:', n1 > 0 ? 'PASS' : 'FAIL (0 bytes)');
} else {
  console.log('TEST 1: FAIL (timeout)');
}

// ── Test 2: COM → companion ──────────────────────────────────────────────

console.log('\n=== TEST 2: COM → companion ===');
compReceived.length = 0;

const wbuf = Buffer.from('Hello companion!\r\n');
const ov2 = mkOv(hEvt2);
const n2b = Buffer.alloc(4);
WriteFile(hCom, wbuf, wbuf.length, n2b, ov2);
WaitForSingleObject(hEvt2, 1000);
const wn = n2b.readUInt32LE(0);
console.log(`COM wrote ${wn} bytes`);

await sleep(1000);
const compAll = Buffer.concat(compReceived).toString();
console.log('Companion received:', JSON.stringify(compAll));
console.log('TEST 2:', compAll.includes('Hello companion!') ? 'PASS' : 'FAIL');

// ── Cleanup ──────────────────────────────────────────────────────────────
CloseHandle(hEvt1); CloseHandle(hEvt2); CloseHandle(hCom);
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
