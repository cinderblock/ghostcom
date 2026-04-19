// Test: does SleepEx(alertable) in a Bun Worker receive APCs?
// Uses QueueUserAPC to send an APC to THIS thread from a native thread.
import { dlopen, FFIType, JSCallback } from 'bun:ffi';

const lib = dlopen('kernel32.dll', {
  GetCurrentThread:   { args: [], returns: FFIType.pointer },
  GetCurrentThreadId: { args: [], returns: FFIType.u32 },
  OpenThread:         { args: ['u32','bool','u32'], returns: FFIType.pointer },
  QueueUserAPC:       { args: ['ptr', 'ptr', 'usize'], returns: FFIType.bool },
  SleepEx:            { args: ['u32', 'bool'], returns: FFIType.u32 },
  CloseHandle:        { args: ['ptr'], returns: FFIType.bool },
  GetLastError:       { args: [], returns: FFIType.u32 },
});
const { GetCurrentThread, GetCurrentThreadId, OpenThread, QueueUserAPC, SleepEx, CloseHandle, GetLastError } = lib.symbols;

self.postMessage({ type: 'start' });

// Get a REAL thread handle (not a pseudo-handle) so QueueUserAPC works
const tid = GetCurrentThreadId();
const THREAD_SET_CONTEXT = 0x0010;
const hThread = OpenThread(THREAD_SET_CONTEXT, false, tid);
self.postMessage({ type: 'thread', tid, handle: hThread?.toString() });

let apcCallCount = 0;

// APC routine: just increments a counter
const apcRoutine = new JSCallback(
  (_param) => {
    apcCallCount++;
    self.postMessage({ type: 'apc_fired', count: apcCallCount });
  },
  { args: ['usize'], returns: 'void', threadsafe: true }
);

// Queue an APC to THIS thread using QueueUserAPC.
// Requires a real thread handle (not pseudo-handle).
const queueOk = QueueUserAPC(apcRoutine.ptr, hThread, 42n);
const queueErr = GetLastError();
self.postMessage({ type: 'queued_apc', ok: queueOk, err: queueErr });

// Now call SleepEx(alertable) - the APC should fire and SleepEx should return 192
self.postMessage({ type: 'entering_sleepex' });
const result = SleepEx(500, true);  // 500ms timeout, alertable=true
// Wait 200ms to give Bun's event loop time to process any scheduled callbacks
await new Promise(r => setTimeout(r, 200));
self.postMessage({ type: 'sleepex_returned', result, expected: 192, apcCallCount });

CloseHandle(hThread);
apcRoutine.close();
self.postMessage({ type: 'done' });
