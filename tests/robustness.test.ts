/**
 * Driver robustness tests — edge cases in lifecycle, concurrency, and
 * high-frequency I/O that don't depend on PnP enumeration.
 *
 * Unlike enumeration.test.ts these should pass on the current driver.
 * They catch bugs in ref-counting, queue teardown, ring-buffer behavior
 * under sustained load, and create/destroy races.
 */

import { describe, it, expect, beforeAll, afterAll, beforeEach } from "bun:test";
import { createRequire } from "node:module";
import { dlopen, FFIType } from "bun:ffi";
import path from "node:path";

const addonPath = path.resolve(import.meta.dir, "../addon/ghostcom.node");
let addonAvailable = false;
try {
  createRequire(import.meta.url)(addonPath);
  addonAvailable = true;
} catch { /* addon not built */ }

const SKIP_MSG = "SKIP: addon not built — run `bun run build:addon` first";

// ── Win32 helpers ────────────────────────────────────────────────────────────

const win32 = dlopen("kernel32.dll", {
  CreateFileW:         { args: ["ptr","u32","u32","ptr","u32","u32","ptr"], returns: FFIType.pointer },
  CloseHandle:         { args: ["ptr"], returns: FFIType.bool },
  CreateEventW:        { args: ["ptr","bool","bool","ptr"], returns: FFIType.pointer },
  ReadFile:            { args: ["ptr","ptr","u32","ptr","ptr"], returns: FFIType.bool },
  WriteFile:           { args: ["ptr","ptr","u32","ptr","ptr"], returns: FFIType.bool },
  WaitForSingleObject: { args: ["ptr","u32"], returns: FFIType.u32 },
  GetOverlappedResult: { args: ["ptr","ptr","ptr","bool"], returns: FFIType.bool },
  GetLastError:        { args: [], returns: FFIType.u32 },
});
const {
  CreateFileW, CloseHandle, CreateEventW,
  ReadFile, WriteFile, WaitForSingleObject, GetOverlappedResult, GetLastError,
} = win32.symbols;

function enc16(s: string): Buffer {
  const b = Buffer.alloc((s.length + 1) * 2);
  for (let i = 0; i < s.length; i++) b.writeUInt16LE(s.charCodeAt(i), i * 2);
  return b;
}
function mkOv(hEvent: unknown): Buffer {
  const ov = Buffer.alloc(32);
  ov.writeBigUInt64LE(BigInt((hEvent as any).valueOf?.() ?? hEvent), 24);
  return ov;
}
function openCom(port: number): unknown {
  return CreateFileW(enc16(`\\\\.\\COM${port}`), 0xC0000000, 3, null, 3, 0x40000000, null);
}
const INVALID_HANDLE = 0xFFFFFFFFFFFFFFFFn;
function isValidHandle(h: unknown): boolean {
  const v = BigInt((h as any).valueOf?.() ?? h);
  return v !== 0n && v !== INVALID_HANDLE;
}

interface NativeStream {
  onData(cb: (c: Buffer) => void): void;
  onReadError(cb: (m: string) => void): void;
  resumeReading(): void;
  write(b: Buffer, cb: (e?: Error | null) => void): void;
  shutdown(): void;
}
interface NativePort {
  createStream(): NativeStream;
  onSignalChange(cb: (r: any) => void): void;
  shutdownSignals(): void;
  close(): void;
}
interface NativeAddon {
  createPort(n: number): { portNumber: number; companionIndex: number };
  openPort(ci: number): NativePort;
  destroyPort(ci: number): void;
  listPorts(): Array<{ portNumber: number; companionIndex: number }>;
}

function sleep(ms: number) { return new Promise<void>(r => setTimeout(r, ms)); }

describe("GhostCOM — driver robustness", () => {
  let native: NativeAddon;

  beforeAll(() => {
    if (!addonAvailable) return;
    native = createRequire(import.meta.url)(addonPath) as NativeAddon;
  });

  // process.exit removed — caused stale symlinks.

  beforeEach(async () => {
    if (!addonAvailable) return;
    for (const p of native.listPorts()) {
      try { native.destroyPort(p.companionIndex); } catch {}
    }
    await sleep(300);
  });

  it("rapid create/destroy cycle leaves no leaked ports", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const baseline = native.listPorts().length;
    for (let i = 0; i < 10; i++) {
      const { companionIndex } = native.createPort(0);
      await sleep(20);
      native.destroyPort(companionIndex);
      await sleep(20);
    }
    await sleep(200);
    expect(native.listPorts().length).toBe(baseline);
  });

  it("closing COM handle without destroying port allows reopen", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const { portNumber, companionIndex } = native.createPort(0);
    const port = native.openPort(companionIndex);
    const stream = port.createStream();
    stream.onData(() => {});
    stream.onReadError(() => {});
    stream.resumeReading();
    port.onSignalChange(() => {});
    await sleep(200);

    // First open/close cycle on the COM side.
    const h1 = openCom(portNumber);
    expect(isValidHandle(h1)).toBe(true);
    CloseHandle(h1);
    await sleep(100);

    // Second open should succeed — driver must re-arm after the first close.
    const h2 = openCom(portNumber);
    expect(isValidHandle(h2)).toBe(true);
    CloseHandle(h2);
    await sleep(100);

    stream.shutdown();
    port.shutdownSignals();
    port.close();
    await sleep(100);
    native.destroyPort(companionIndex);
  });

  it("1000 small writes (10 bytes each) arrive intact and in order", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const { portNumber, companionIndex } = native.createPort(0);
    const port = native.openPort(companionIndex);
    const stream = port.createStream();
    stream.onData(() => {});
    stream.onReadError(() => {});
    stream.resumeReading();
    port.onSignalChange(() => {});
    await sleep(200);

    const hCom = openCom(portNumber);
    expect(isValidHandle(hCom)).toBe(true);

    // Reader thread — pulls from COM side while writer pushes from companion.
    const N_WRITES = 1000;
    const CHUNK_LEN = 10;
    const TOTAL = N_WRITES * CHUNK_LEN;
    const received = Buffer.alloc(TOTAL);
    let recvOff = 0;
    const hEvt = CreateEventW(null, true, false, null);

    const readOnce = () => new Promise<number>((resolve) => {
      const ov = mkOv(hEvt);
      const remaining = TOTAL - recvOff;
      const tmp = Buffer.alloc(Math.min(4096, remaining));
      ReadFile(hCom, tmp, tmp.length, null, ov);
      (function poll() {
        const w = WaitForSingleObject(hEvt, 0);
        if (w === 0) {
          const nb = Buffer.alloc(4);
          GetOverlappedResult(hCom, ov, nb, false);
          const n = nb.readUInt32LE(0);
          tmp.copy(received, recvOff, 0, n);
          recvOff += n;
          resolve(n);
        } else {
          setTimeout(poll, 5);
        }
      })();
    });

    // Kick a background drain loop.
    let draining = true;
    const drainPromise = (async () => {
      while (draining && recvOff < TOTAL) {
        await readOnce();
      }
    })();

    // Fire 1000 writes, each 10 bytes, sequentially via companion.
    const expected = Buffer.alloc(TOTAL);
    for (let i = 0; i < N_WRITES; i++) {
      const chunk = Buffer.alloc(CHUNK_LEN);
      for (let j = 0; j < CHUNK_LEN; j++) chunk[j] = (i * 31 + j * 7) & 0xff;
      chunk.copy(expected, i * CHUNK_LEN);
      await new Promise<void>((res, rej) =>
        stream.write(chunk, (e) => (e ? rej(e) : res())),
      );
    }

    // Wait up to 5s for the drain to finish.
    const deadline = Date.now() + 5000;
    while (recvOff < TOTAL && Date.now() < deadline) await sleep(20);
    draining = false;
    await drainPromise.catch(() => {});

    expect(recvOff).toBe(TOTAL);
    expect(received.equals(expected)).toBe(true);

    CloseHandle(hEvt);
    CloseHandle(hCom);
    stream.shutdown();
    port.shutdownSignals();
    port.close();
    await sleep(100);
    native.destroyPort(companionIndex);
  }, 15_000);

  it("destroyPort with live COM handle does not crash or leak", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const { portNumber, companionIndex } = native.createPort(0);
    const port = native.openPort(companionIndex);
    const stream = port.createStream();
    stream.onData(() => {});
    stream.onReadError(() => {});
    stream.resumeReading();
    port.onSignalChange(() => {});
    await sleep(200);

    const hCom = openCom(portNumber);
    expect(isValidHandle(hCom)).toBe(true);

    // Post a pending read so the driver has in-flight I/O.
    const hEvt = CreateEventW(null, true, false, null);
    const ov = mkOv(hEvt);
    const rbuf = Buffer.alloc(64);
    ReadFile(hCom, rbuf, rbuf.length, null, ov);
    await sleep(100);

    // Tear down the addon side first …
    stream.shutdown();
    port.shutdownSignals();
    port.close();
    await sleep(50);

    // … then destroy the port while the COM handle is still open with a
    // pended read.  The driver must cancel the IRP cleanly and not BSOD.
    native.destroyPort(companionIndex);
    await sleep(300);

    // Pending read should have completed (with ERROR_OPERATION_ABORTED or
    // similar) — wait briefly so we don't leak the event.
    WaitForSingleObject(hEvt, 500);

    CloseHandle(hEvt);
    CloseHandle(hCom);

    // Port must be gone from listPorts().
    const still = native.listPorts().find(p => p.companionIndex === companionIndex);
    expect(still).toBeUndefined();
  });

  it("second openPort on same companion index fails with a clean error", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const { companionIndex } = native.createPort(0);
    const port1 = native.openPort(companionIndex);
    await sleep(100);

    // Second open while the first is still held should error, not BSOD
    // and not silently return a second handle to the same device.
    let err: Error | null = null;
    let port2: NativePort | null = null;
    try {
      port2 = native.openPort(companionIndex);
    } catch (e) {
      err = e as Error;
    }

    // Accept either: (a) openPort threw, or (b) returned a second handle
    // but the underlying driver rejected it. (a) is the clean outcome we
    // want; log if (b) happens so we notice.
    if (!err) {
      console.warn("openPort did NOT reject second open — driver may be allowing dup opens");
    }
    expect(err).not.toBeNull();

    try { port2?.close?.(); } catch {}
    port1.close();
    await sleep(100);
    native.destroyPort(companionIndex);
  });
});
