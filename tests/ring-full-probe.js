/**
 * Ring-full boundary probe.
 *
 * Writes exactly GCOM_RING_BUFFER_SIZE - 1 = 65535 bytes companion → COM.
 * With Driver Verifier enabled on ghostcom.sys, any kernel bug in the
 * ring buffer / drain path should produce a specific bugcheck dump at
 * C:\Windows\MEMORY.DMP instead of silently BSODing.
 *
 * Writes progress to stderr after every step so the last printed line
 * tells us exactly where the driver crashed.
 */
import { createRequire } from 'node:module';
import { dlopen, FFIType } from 'bun:ffi';

const log = (msg) => { process.stderr.write(`[probe] ${msg}\n`); };

log('loading addon');
const req = createRequire(import.meta.url);
const native = req('../addon/ghostcom.node');

log('cleanup stale ports');
for (const p of native.listPorts()) {
  try { native.destroyPort(p.companionIndex); } catch {}
}
await new Promise(r => setTimeout(r, 500));

log('creating COM10');
const { portNumber, companionIndex } = native.createPort(10);
log(`  → portNumber=${portNumber}, companionIndex=${companionIndex}`);

log('opening companion');
const port = native.openPort(companionIndex);
const stream = port.createStream();
stream.onData(() => {});
stream.onReadError(e => log(`stream error: ${e}`));
stream.resumeReading();
port.onSignalChange(() => {});
await new Promise(r => setTimeout(r, 100));

log('opening Win32 COM handle (FFI)');
const lib = dlopen('kernel32.dll', {
  CreateFileW:         { args: ['ptr','u32','u32','ptr','u32','u32','ptr'], returns: FFIType.pointer },
  CloseHandle:         { args: ['ptr'], returns: FFIType.bool },
  CreateEventW:        { args: ['ptr','bool','bool','ptr'], returns: FFIType.pointer },
  ReadFile:            { args: ['ptr','ptr','u32','ptr','ptr'], returns: FFIType.bool },
  WaitForSingleObject: { args: ['ptr','u32'], returns: FFIType.u32 },
  GetOverlappedResult: { args: ['ptr','ptr','ptr','bool'], returns: FFIType.bool },
});
const { CreateFileW, CloseHandle, CreateEventW, ReadFile, WaitForSingleObject, GetOverlappedResult } = lib.symbols;

const encode16 = (s) => {
  const b = Buffer.alloc((s.length + 1) * 2);
  for (let i = 0; i < s.length; i++) b.writeUInt16LE(s.charCodeAt(i), i * 2);
  return b;
};

const hCom = CreateFileW(encode16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);
log('COM handle opened');
await new Promise(r => setTimeout(r, 100));

log('creating OVERLAPPED for ReadFile');
const hEvt = CreateEventW(null, true, false, null);
const ov = Buffer.alloc(32);
ov.writeBigUInt64LE(BigInt(hEvt.valueOf?.() ?? hEvt), 24);

const RING_MAX = 65535;
log(`allocating ${RING_MAX}-byte read buffer`);
const rbuf = Buffer.alloc(RING_MAX + 512);

log('issuing overlapped ReadFile (ring is empty — will pend)');
ReadFile(hCom, rbuf, rbuf.length, null, ov);
await new Promise(r => setTimeout(r, 100));

log(`preparing ${RING_MAX}-byte payload`);
const payload = Buffer.alloc(RING_MAX);
for (let i = 0; i < RING_MAX; i++) payload[i] = (i * 7) & 0xff;

log(`writing ${RING_MAX} bytes companion → COM (THIS IS THE DANGEROUS STEP)`);
await new Promise((res, rej) =>
  stream.write(payload, (e) => e ? rej(e) : res())
);
log('companion write completed');

log('waiting for COM ReadFile to complete');
const w = WaitForSingleObject(hEvt, 10_000);
log(`  WaitForSingleObject returned: ${w} (0=signaled, 258=timeout)`);

if (w === 0) {
  const nb = Buffer.alloc(4);
  GetOverlappedResult(hCom, ov, nb, false);
  const n = nb.readUInt32LE(0);
  log(`  received ${n} bytes on COM side`);
  log(`  first byte=${rbuf[0]}, expected ${payload[0]}`);
  log(`  last byte=${rbuf[n-1]}, expected ${payload[n-1]}`);
  log(`  bytes match exactly: ${rbuf.slice(0, n).equals(payload)}`);
}

log('cleanup');
CloseHandle(hEvt);
CloseHandle(hCom);
stream.shutdown();
port.shutdownSignals();
port.close();
await new Promise(r => setTimeout(r, 200));
native.destroyPort(companionIndex);
log('DONE — no crash');
