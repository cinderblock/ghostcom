// Diagnostic 6: test async COM read (issued BEFORE companion write)
// Uses Bun FFI with FILE_FLAG_OVERLAPPED to test the pending-read drain path
import { createRequire } from 'node:module';
import { dlopen, FFIType, ptr } from 'bun:ffi';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');
for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Port: COM${portNumber}, ci=${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();
const received = [];
stream.onData(c => { received.push(c); console.log(`[comp<-COM] ${c.length}b`); });
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();
port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

const lib = dlopen('kernel32.dll', {
  CreateFileW: { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:  { args: ['ptr'], returns: FFIType.bool },
  CreateEventW: { args: ['ptr','bool','bool','ptr'], returns: FFIType.pointer },
  ReadFile:     { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  WaitForSingleObject: { args: ['ptr','u32'], returns: FFIType.u32 },
  GetOverlappedResult: { args: ['ptr','ptr','ptr','bool'], returns: FFIType.bool },
  GetLastError: { args: [], returns: FFIType.u32 },
  WriteFile:    { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
});
const { CreateFileW, CloseHandle, CreateEventW, ReadFile, WaitForSingleObject, GetOverlappedResult, GetLastError, WriteFile } = lib.symbols;

const encode16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };
const comPath = encode16(`\\\\.\\COM${portNumber}`);

// Open COM with FILE_FLAG_OVERLAPPED (0x40000000)
const hCom = CreateFileW(comPath, 0xC0000000, 3, null, 3, 0x40000000, null);
const err0 = GetLastError();
console.log(`COM opened: handle ptr, err=${err0}`);

// Create OVERLAPPED event
const hEvent = CreateEventW(null, true, false, null);

// OVERLAPPED structure layout on x64 (32 bytes total):
//   0: Internal (8 bytes)
//   8: InternalHigh (8 bytes)
//  16: Offset/OffsetHigh union (8 bytes)
//  24: hEvent (8 bytes)  ← event handle goes here
const ovBuf = Buffer.alloc(32);
const evPtr = BigInt(hEvent.valueOf ? hEvent.valueOf() : hEvent);
ovBuf.writeBigUInt64LE(evPtr, 24); // hEvent at offset 24

const readBuf = Buffer.alloc(64);
const bytesReadBuf = Buffer.alloc(4);

// Issue overlapped ReadFile BEFORE companion writes
console.log('\nIssuing overlapped ReadFile (ring is empty)...');
const readOk = ReadFile(hCom, readBuf, 64, bytesReadBuf, ovBuf);
const readErr = GetLastError();
console.log(`ReadFile immediate: ok=${readOk}, err=${readErr} (997=IO_PENDING)`);

// Wait a moment then write from companion
await sleep(300);
console.log('Writing PING from companion...');
await new Promise((res, rej) => stream.write(Buffer.from('PING\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote PING');

// Wait for the overlapped read to complete (max 3s)
console.log('Waiting for COM read to complete...');
const waitResult = WaitForSingleObject(hEvent, 3000);
console.log(`WaitForSingleObject: ${waitResult} (0=signaled)`);

if (waitResult === 0) {
  const n2Buf = Buffer.alloc(4);
  const gotOk = GetOverlappedResult(hCom, ovBuf, n2Buf, false);
  const n2 = n2Buf.readUInt32LE(0);
  console.log(`GetOverlappedResult: ok=${gotOk}, n=${n2}`);
  if (n2 > 0) {
    console.log(`COM received: "${readBuf.slice(0, n2).toString()}"`);
  }
} else {
  console.log('Timed out — data never arrived at COM side');
}

CloseHandle(hCom);
CloseHandle(hEvent);
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
