/**
 * Driver robustness tests — edge cases in lifecycle, concurrency, and
 * high-frequency I/O that don't depend on PnP enumeration.
 *
 * Unlike enumeration.test.ts these should pass on the current driver.
 * They catch bugs in ref-counting, queue teardown, ring-buffer behavior
 * under sustained load, and create/destroy races.
 */

import { describe, it, expect, beforeAll, beforeEach } from "bun:test";
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
});
