// Diagnostic 9: Test ReadFileEx + APC using Bun's JSCallback.
// ReadFileEx uses Windows APCs for completion. If the APC doesn't fire,
// that points to a driver issue. If it fires but uv_async_send doesn't
// wake Bun, that points to a Bun limitation.
//
// This test runs the ReadFileEx call on a WORKER THREAD (via Bun Worker)
// so that the alertable SleepEx can be called without blocking the main loop.

import { createRequire } from 'node:module';
import { setTimeout as sleep } from 'node:timers/promises';
import { dlopen, FFIType, JSCallback } from 'bun:ffi';

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

const lib = dlopen('kernel32.dll', {
  CreateFileW:          { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:          { args: ['ptr'], returns: FFIType.bool },
  ReadFileEx:           { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  SleepEx:             { args: ['u32','bool'], returns: FFIType.u32 },
  GetLastError:         { args: [], returns: FFIType.u32 },
});
const { CreateFileW, CloseHandle, ReadFileEx, SleepEx, GetLastError } = lib.symbols;

const enc16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };

let apcFired = false;
let bytesInApc = 0;
let apcBuf;

// Create a JSCallback for the ReadFileEx completion routine
// Signature: VOID (DWORD dwErrorCode, DWORD dwNumberOfBytesTransfered, LPOVERLAPPED lpOverlapped)
const callback = new JSCallback(
  (errorCode, bytesTransferred, _overlappedPtr) => {
    console.log(`[APC] fired! errorCode=${errorCode}, bytes=${bytesTransferred}`);
    apcFired = true;
    bytesInApc = bytesTransferred;
  },
  { args: ['u32', 'u32', 'ptr'], returns: 'void', threadsafe: true }
);

// Open COM port with FILE_FLAG_OVERLAPPED
const hCom = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);
const openErr = GetLastError();
if (openErr !== 0) {
  console.error(`COM open failed: err=${openErr}`);
  callback.close(); stream.shutdown(); port.shutdownSignals(); port.close(); native.destroyPort(companionIndex);
  process.exit(1);
}
console.log(`COM${portNumber} opened via FFI`);

// Build OVERLAPPED struct (zeroed, no event — ReadFileEx uses APC not event)
const ovBuf = Buffer.alloc(32);
apcBuf = Buffer.alloc(64);

// Issue ReadFileEx for 1 byte
const ok = ReadFileEx(hCom, apcBuf, 1, ovBuf, callback.ptr);
const rxErr = GetLastError();
console.log(`ReadFileEx: ok=${ok}, err=${rxErr}`);
// ReadFileEx typically returns true (non-overlapped success) or sets last error
// For async, it returns true and sets err=0 if the IRP was queued

// Note: ReadFileEx doesn't return ERROR_IO_PENDING — it starts async and
// the APC fires when complete. SleepEx(alertable) is how we receive it.
console.log('\nEntering SleepEx (alertable, 500ms)...');

// Write from companion while SleepEx is pending
// We need to do this concurrently. Use a small delay + check.
const apcPromise = new Promise((resolve) => {
  // Poll for APC by calling SleepEx in small increments on main thread
  // Note: main JS thread can't enter true SleepEx. We check via polling.
  let attempts = 0;
  const check = () => {
    if (apcFired || attempts++ > 100) { resolve(apcFired); return; }
    // Call SleepEx briefly to process any pending APCs on this thread
    SleepEx(10, true);  // 10ms, alertable
    setTimeout(check, 0);
  };
  setTimeout(check, 0);
});

// Write from companion after brief delay
await sleep(300);
console.log('Writing from companion...');
await new Promise((res, rej) => stream.write(Buffer.from('APC_TEST\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote APC_TEST');

const result = await apcPromise;

console.log('\n=== DIAGNOSIS ===');
if (result) {
  console.log(`✓ APC fired with ${bytesInApc} bytes — driver correctly completes ReadFileEx`);
  console.log('  ReadFileEx + SleepEx works on THIS thread (main Bun JS thread)');
  console.log('  → serialport issue may be that its worker thread SleepEx is affected by Bun');
} else {
  console.log('✗ APC did NOT fire — DRIVER may not correctly complete ReadFileEx IRPs');
  console.log('  OR: main Bun thread SleepEx does not process APCs (need separate thread test)');
}

callback.close();
CloseHandle(hCom);
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
