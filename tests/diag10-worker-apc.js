// Diagnostic 10: Does ReadFileEx APC fire on a CreateThread worker thread?
// This simulates exactly what serialport does:
//   1. CreateThread a worker
//   2. Worker opens COM, issues ReadFileEx, calls SleepEx(INFINITE, alertable)
//   3. Main process writes from companion
//   4. Check if APC fired on worker thread
//
// If APC fires on worker → serialport SHOULD work → it's a uv_async_send issue in Bun
// If APC doesn't fire on worker → driver or Windows APC issue (deep bug)
import { createRequire } from 'node:module';
import { setTimeout as sleep } from 'node:timers/promises';
import { dlopen, FFIType, JSCallback, ptr } from 'bun:ffi';

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
  CreateFileW:      { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:      { args: ['ptr'], returns: FFIType.bool },
  ReadFileEx:       { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  SleepEx:          { args: ['u32','bool'], returns: FFIType.u32 },
  GetLastError:     { args: [], returns: FFIType.u32 },
  CreateThread:     { args: ['ptr','usize','ptr','ptr','u32','ptr'], returns: FFIType.pointer },
  WaitForSingleObject: { args: ['ptr','u32'], returns: FFIType.u32 },
  CreateEventW:     { args: ['ptr','bool','bool','ptr'], returns: FFIType.pointer },
  SetEvent:         { args: ['ptr'], returns: FFIType.bool },
});
const { CreateFileW, CloseHandle, ReadFileEx, SleepEx, GetLastError, CreateThread, WaitForSingleObject, CreateEventW, SetEvent } = lib.symbols;

const enc16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };

// Shared state between threads
const sharedBuf = Buffer.alloc(64); // read buffer
const ovBuf = Buffer.alloc(32);     // OVERLAPPED (zeroed, no event — APC mode)
let apcFired = false;
let apcBytes = 0;

// APC completion callback (fires on whatever thread called ReadFileEx)
const apcCallback = new JSCallback(
  (errorCode, bytesTransferred, _ov) => {
    console.log(`[APC on worker] errorCode=${errorCode}, bytes=${bytesTransferred}`);
    apcFired = true;
    apcBytes = bytesTransferred;
  },
  { args: ['u32', 'u32', 'ptr'], returns: 'void', threadsafe: true }
);

// Event to signal "worker is ready (ReadFileEx issued)"
const hReady = CreateEventW(null, false, false, null);

// Thread function: opens COM, issues ReadFileEx, signals ready, enters SleepEx
const threadFn = new JSCallback(
  (_param) => {
    const h = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);
    if (GetLastError() !== 0 && GetLastError() !== 183) {
      console.error(`Worker: CreateFile failed err=${GetLastError()}`);
      SetEvent(hReady);
      return null;
    }
    console.log('[worker] COM opened');

    const ok = ReadFileEx(h, sharedBuf, 1, ovBuf, apcCallback.ptr);
    console.log(`[worker] ReadFileEx: ok=${ok}, err=${GetLastError()}`);

    // Signal main thread that we're ready
    SetEvent(hReady);

    // Enter alertable wait — APC should fire here when data arrives
    console.log('[worker] Entering SleepEx(INFINITE, alertable)...');
    const r = SleepEx(8000, true); // 8 second timeout, alertable=true
    console.log(`[worker] SleepEx returned: ${r} (192=APC_FIRED, 0=timeout)`);

    CloseHandle(h);
    return null;
  },
  { args: ['ptr'], returns: 'ptr', threadsafe: true }
);

// Start the worker thread
const hThread = CreateThread(null, 0, threadFn.ptr, null, 0, null);
console.log('Worker thread started');

// Wait for worker to be ready (ReadFileEx issued)
const waitReady = WaitForSingleObject(hReady, 5000);
if (waitReady !== 0) {
  console.error('Worker thread did not become ready!');
  process.exit(1);
}
console.log('Worker ready. Writing from companion...');
await sleep(200);

await new Promise((res, rej) => stream.write(Buffer.from('WORKER_TEST\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote WORKER_TEST');

// Wait for worker thread to finish
const waitThread = WaitForSingleObject(hThread, 10000);
console.log(`Thread wait result: ${waitThread}`);

console.log('\n=== DIAGNOSIS ===');
if (apcFired) {
  console.log(`✓ APC fired on worker thread with ${apcBytes} bytes`);
  console.log('  → Driver correctly queues APCs for worker threads');
  console.log('  → serialport issue is NOT in APC delivery');
  console.log('  → issue is likely uv_async_send() not waking Bun from CreateThread worker');
  console.log('  → this is a Bun compatibility issue with serialport');
} else {
  console.log('✗ APC did NOT fire on worker thread');
  console.log('  → Could be: driver bug, Windows APC delivery issue, or thread state');
}

CloseHandle(hThread);
CloseHandle(hReady);
apcCallback.close();
threadFn.close();
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);
