// Diagnostic 5: use Bun FFI to call CreateFile directly
import { createRequire } from 'node:module';
import { dlopen, FFIType, CString } from 'bun:ffi';
import { setTimeout as sleep } from 'node:timers/promises';

const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');

// Clean up
for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
await sleep(200);

const { portNumber, companionIndex } = native.createPort(0);
console.log(`Port: COM${portNumber}, ci=${companionIndex}`);

const port = native.openPort(companionIndex);
const stream = port.createStream();
const received = [];
stream.onData(chunk => { received.push(chunk); console.log(`[companion<-COM] ${chunk.length}b`); });
stream.onReadError(e => console.error('[read err]', e));
stream.resumeReading();
port.onSignalChange(s => console.log(`[signal] 0x${s.changedMask.toString(16)}`));
await sleep(100);

// Use Bun FFI to call CreateFile and get the real error
const { symbols: { CreateFileW, GetLastError, CloseHandle, ReadFile, WriteFile } } = dlopen(
  'kernel32.dll',
  {
    CreateFileW: {
      args: ['ptr', 'u32', 'u32', 'ptr', 'u32', 'u32', 'ptr'],
      returns: FFIType.pointer,
    },
    GetLastError: { args: [], returns: FFIType.u32 },
    CloseHandle: { args: ['ptr'], returns: FFIType.bool },
    ReadFile: { args: ['ptr', 'ptr', 'u32', 'ptr', 'ptr'], returns: FFIType.bool },
    WriteFile: { args: ['ptr', 'ptr', 'u32', 'ptr', 'ptr'], returns: FFIType.bool },
  }
);

// Encode path as UTF-16LE
const pathStr = `\\\\.\\COM${portNumber}`;
const pathBuf = Buffer.alloc((pathStr.length + 1) * 2);
for (let i = 0; i < pathStr.length; i++) pathBuf.writeUInt16LE(pathStr.charCodeAt(i), i * 2);
pathBuf.writeUInt16LE(0, pathStr.length * 2);

// Call CreateFile(path, GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL)
const GENERIC_RW = 0xC0000000;
const SHARE_RW = 0x00000003;  // FILE_SHARE_READ | FILE_SHARE_WRITE
const OPEN_EXISTING = 3;
const handle = CreateFileW(pathBuf, GENERIC_RW, SHARE_RW, null, OPEN_EXISTING, 0, null);
const err = GetLastError();
const INVALID = BigInt('0xFFFFFFFFFFFFFFFF'); // INVALID_HANDLE_VALUE for 64-bit

console.log(`CreateFile handle=0x${BigInt(handle.valueOf?.() ?? handle).toString(16)}, err=${err}`);

if (err !== 0 || handle == null) {
  console.log(`FAILED. Win32 error: ${err} (${winErrName(err)})`);
  stream.shutdown(); port.shutdownSignals(); port.close();
  native.destroyPort(companionIndex);
  process.exit(1);
}

console.log('COM10 opened via FFI!');

// Write from companion -> COM
console.log('\nTest: companion->COM');
await new Promise((res, rej) => stream.write(Buffer.from('HELLO\r\n'), e => e ? rej(e) : res()));
console.log('Companion wrote HELLO');

// Async read from COM side using ReadFile with timeout
await sleep(1000);

// Read bytes from COM side (if any arrived synchronously)
const readBuf = Buffer.alloc(64);
const bytesReadBuf = Buffer.alloc(4);
const readOk = ReadFile(handle, readBuf, 64, bytesReadBuf, null);
const n = bytesReadBuf.readUInt32LE(0);
const readErr = GetLastError();
console.log(`ReadFile: ok=${readOk}, n=${n}, err=${readErr}`);
if (n > 0) console.log('COM received:', readBuf.slice(0, n).toString());

CloseHandle(handle);
stream.shutdown(); port.shutdownSignals(); port.close();
native.destroyPort(companionIndex);

function winErrName(code) {
  const names = { 0: 'SUCCESS', 2: 'FILE_NOT_FOUND', 5: 'ACCESS_DENIED', 31: 'GEN_FAILURE', 32: 'SHARING_VIOLATION', 33: 'LOCK_VIOLATION', 183: 'ALREADY_EXISTS' };
  return names[code] || `UNKNOWN(${code})`;
}
