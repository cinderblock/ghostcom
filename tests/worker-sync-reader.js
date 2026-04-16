/**
 * Bun Worker: opens a COM port WITHOUT FILE_FLAG_OVERLAPPED (synchronous I/O)
 * and does a blocking ReadFile.  Used by the synchronous-read e2e test.
 *
 * Message protocol:
 *   recv { portNumber, timeoutMs }   → opens port, calls ReadFile (blocks)
 *   send { type:'open' }             → port is open, ReadFile is about to issue
 *   send { type:'data', hex }        → data received
 *   send { type:'error', msg }       → ReadFile failed or timed out
 */
import { dlopen, FFIType } from 'bun:ffi';

const lib = dlopen('kernel32.dll', {
  CreateFileW:    { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:    { args: ['ptr'], returns: FFIType.bool },
  ReadFile:       { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  SetCommTimeouts:{ args: ['ptr','ptr'], returns: FFIType.bool },
  GetLastError:   { args: [], returns: FFIType.u32 },
});
const { CreateFileW, CloseHandle, ReadFile, SetCommTimeouts, GetLastError } = lib.symbols;

const enc16 = s => { const b = Buffer.alloc((s.length+1)*2); for(let i=0;i<s.length;i++) b.writeUInt16LE(s.charCodeAt(i),i*2); return b; };

// Receive config from main thread
const { portNumber, timeoutMs } = await new Promise(res => { self.onmessage = e => { self.onmessage = null; res(e.data); }; });

// Open WITHOUT FILE_FLAG_OVERLAPPED — this is the "synchronous I/O" path
const hCom = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0, null);
if (GetLastError() !== 0) {
  self.postMessage({ type: 'error', msg: `CreateFile failed: err=${GetLastError()}` });
  process.exit(1);
}

// Set a total read timeout so the test doesn't hang forever
const timeouts = Buffer.alloc(20);           // COMMTIMEOUTS (5 DWORDs)
timeouts.writeUInt32LE(0,  0);               // ReadIntervalTimeout = 0 (block)
timeouts.writeUInt32LE(0,  4);               // ReadTotalTimeoutMultiplier = 0
timeouts.writeUInt32LE(timeoutMs, 8);        // ReadTotalTimeoutConstant = N ms
timeouts.writeUInt32LE(0,  12);              // WriteTotalTimeoutMultiplier = 0
timeouts.writeUInt32LE(0,  16);              // WriteTotalTimeoutConstant = 0
SetCommTimeouts(hCom, timeouts);

// Signal main thread: port is open, about to block on ReadFile
self.postMessage({ type: 'open' });

// Blocking ReadFile — will return when data arrives or timeout expires
const buf = Buffer.alloc(128);
const n = Buffer.alloc(4);
const ok = ReadFile(hCom, buf, 128, n, null); // null OVERLAPPED = synchronous
const err = GetLastError();
const count = n.readUInt32LE(0);

CloseHandle(hCom);

if (!ok && count === 0) {
  self.postMessage({ type: 'error', msg: `ReadFile failed or timed out: ok=${ok} err=${err}` });
} else {
  self.postMessage({ type: 'data', hex: buf.slice(0, count).toString('hex'), text: buf.slice(0, count).toString(), count });
}
