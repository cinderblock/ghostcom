// Bun Worker: opens COM port, issues ReadFileEx on worker's OS thread,
// enters SleepEx(alertable), waits for APC.
// FFI calls in a Bun Worker should run on the worker thread directly.

import { dlopen, FFIType, JSCallback } from 'bun:ffi';

// Wait for config from main thread via first message
const config = await new Promise(res => { self.onmessage = e => { self.onmessage = null; res(e.data); }; });
const { portNumber, comPath } = config;

const lib = dlopen('kernel32.dll', {
  CreateFileW:  { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:  { args: ['ptr'], returns: FFIType.bool },
  ReadFileEx:   { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  SleepEx:      { args: ['u32','bool'], returns: FFIType.u32 },
  GetLastError: { args: [], returns: FFIType.u32 },
});
const { CreateFileW, CloseHandle, ReadFileEx, SleepEx, GetLastError } = lib.symbols;

const enc16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };

postMessage({ type: 'status', msg: 'worker started' });

const h = CreateFileW(enc16(comPath), 0xC0000000, 3, null, 3, 0x40000000, null);
const err = GetLastError();
if (err !== 0) {
  postMessage({ type: 'error', msg: `CreateFile failed: err=${err}` });
  process.exit(1);
}
postMessage({ type: 'status', msg: `COM${portNumber} opened` });

const buf = Buffer.alloc(64);
const ov = Buffer.alloc(32);
let apcResult = null;

const callback = new JSCallback(
  (errorCode, bytesTransferred, _ov) => {
    apcResult = { errorCode, bytesTransferred };
    postMessage({ type: 'apc', errorCode, bytesTransferred, data: buf.slice(0, bytesTransferred).toString() });
  },
  { args: ['u32', 'u32', 'ptr'], returns: 'void', threadsafe: true }
);

const ok = ReadFileEx(h, buf, 1, ov, callback.ptr);
postMessage({ type: 'status', msg: `ReadFileEx ok=${ok} err=${GetLastError()}` });

postMessage({ type: 'ready' }); // Signal main thread

// Enter alertable wait — APC fires here if driver queues it for this thread
const r = SleepEx(8000, true); // 8s timeout
postMessage({ type: 'status', msg: `SleepEx returned ${r} (192=APC_FIRED)` });

if (!apcResult) {
  postMessage({ type: 'timeout' });
}

callback.close();
CloseHandle(h);
postMessage({ type: 'done' });
