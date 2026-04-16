/**
 * COM-port API compatibility tests — patterns used by real serial-port tools.
 *
 * Tests the COM-side Windows serial port API surface beyond basic read/write:
 *   - WaitCommEvent (EV_RXCHAR)   — PuTTY, Windows Terminal, .NET SerialPort
 *   - Synchronous ReadFile         — pyserial, simple C programs, Arduino IDE
 *   - GetCommModemStatus           — hardware flow-control polling, modem check
 *
 * Runs in its own process (separate file from e2e.test.ts) to avoid sharing
 * port state with the bidirectional suite.
 *
 * Requires: GhostCOM driver installed, addon built (`bun run build:addon`).
 * Run: bun test
 */

import {
  describe, it, expect, beforeAll, beforeEach, afterEach,
} from "bun:test";
import { createRequire } from "node:module";
import { dlopen, FFIType } from "bun:ffi";
import path from "node:path";

// ── Detect addon ──────────────────────────────────────────────────────────────

const addonPath = path.resolve(import.meta.dir, "../addon/ghostcom.node");
let addonAvailable = false;
try {
  createRequire(import.meta.url)(addonPath);
  addonAvailable = true;
} catch { /* not built */ }

const SKIP_MSG = "SKIP: addon not built — run `bun run build:addon` first";

// ── Win32 helpers ─────────────────────────────────────────────────────────────

const win32 = dlopen("kernel32.dll", {
  CreateFileW:         { args: ["ptr","u32","u32","ptr","u32","u32","ptr"], returns: FFIType.pointer },
  CloseHandle:         { args: ["ptr"], returns: FFIType.bool },
  CreateEventW:        { args: ["ptr","bool","bool","ptr"], returns: FFIType.pointer },
  ReadFile:            { args: ["ptr","ptr","u32","ptr","ptr"], returns: FFIType.bool },
  WriteFile:           { args: ["ptr","ptr","u32","ptr","ptr"], returns: FFIType.bool },
  WaitForSingleObject: { args: ["ptr","u32"], returns: FFIType.u32 },
  GetOverlappedResult: { args: ["ptr","ptr","ptr","bool"], returns: FFIType.bool },
  GetLastError:        { args: [], returns: FFIType.u32 },
  DeviceIoControl:     { args: ["ptr","u32","ptr","u32","ptr","u32","ptr","ptr"], returns: FFIType.bool },
  GetCommModemStatus:  { args: ["ptr","ptr"], returns: FFIType.bool },
});

const {
  CreateFileW, CloseHandle, CreateEventW, ReadFile, WriteFile,
  WaitForSingleObject, GetOverlappedResult, GetLastError,
  DeviceIoControl, GetCommModemStatus,
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

function readOverlapped(hCom: unknown, size: number, timeoutMs: number): Buffer | null {
  const hEvt = CreateEventW(null, true, false, null);
  const ov = mkOv(hEvt);
  const buf = Buffer.alloc(size);
  ReadFile(hCom, buf, size, null, ov);
  const r = WaitForSingleObject(hEvt, timeoutMs);
  CloseHandle(hEvt);
  if (r !== 0) return null;
  const nb = Buffer.alloc(4);
  GetOverlappedResult(hCom, ov, nb, false);
  return buf.slice(0, nb.readUInt32LE(0));
}

// ── Serial IOCTL codes (ntddser.h, FILE_DEVICE_SERIAL_PORT=0x1b) ─────────────
// CTL_CODE(DevType, Fn, Method=0, Access=0) = (DevType<<16)|(Fn<<2)|0
const ctlCode = (fn: number) => (0x1b << 16) | (fn << 2);
const IOCTL_SET_WAIT_MASK  = ctlCode(7);   // IOCTL_SERIAL_SET_WAIT_MASK
const IOCTL_WAIT_ON_MASK   = ctlCode(8);   // IOCTL_SERIAL_WAIT_ON_MASK
const IOCTL_SET_BAUD_RATE  = ctlCode(1);   // IOCTL_SERIAL_SET_BAUD_RATE
const IOCTL_GET_BAUD_RATE  = ctlCode(2);   // IOCTL_SERIAL_GET_BAUD_RATE
const IOCTL_SET_LINE_CTRL  = ctlCode(3);   // IOCTL_SERIAL_SET_LINE_CONTROL
const IOCTL_GET_LINE_CTRL  = ctlCode(4);   // IOCTL_SERIAL_GET_LINE_CONTROL
const IOCTL_PURGE          = ctlCode(19);  // IOCTL_SERIAL_PURGE
const IOCTL_GET_COMMSTATUS = ctlCode(20);  // IOCTL_SERIAL_GET_COMMSTATUS

// SERIAL_PURGE flags
const SERIAL_PURGE_TXABORT = 0x0001;
const SERIAL_PURGE_RXABORT = 0x0002;
const SERIAL_PURGE_TXCLEAR = 0x0004;
const SERIAL_PURGE_RXCLEAR = 0x0008;

// Windows modem status bits (WinBase.h)
const SERIAL_CTS_STATE = 0x10;
const SERIAL_DSR_STATE = 0x20;
const SERIAL_DCD_STATE = 0x80; // DCD bit

// ── Types ─────────────────────────────────────────────────────────────────────

interface RawSignal { sequenceNumber: number; changedMask: number; baudRate: number; dtrState: boolean; rtsState: boolean; }
interface NativeStream { onData(cb: (c: Buffer) => void): void; onReadError(cb: (m: string) => void): void; resumeReading(): void; write(b: Buffer, cb: (e?: Error | null) => void): void; shutdown(): void; }
interface NativePort { createStream(): NativeStream; onSignalChange(cb: (r: RawSignal) => void): void; getSignals(): RawSignal; setSignals(d: boolean, r: boolean): void; shutdownSignals(): void; close(): void; }
interface NativeAddon { createPort(n: number): { portNumber: number; companionIndex: number }; openPort(ci: number): NativePort; destroyPort(ci: number): void; listPorts(): Array<{ portNumber: number; companionIndex: number }>; }

function sleep(ms: number) { return new Promise<void>(r => setTimeout(r, ms)); }

// ── Test suite ────────────────────────────────────────────────────────────────

describe("GhostCOM — COM-side API compatibility", () => {
  let native: NativeAddon;
  let companionIndex = -1;
  let portNumber = -1;
  let nativePort: NativePort;
  let nativeStream: NativeStream;
  let hCom: unknown;
  const companionReceived: Buffer[] = [];
  // Shared signal log — beforeEach resets it; tests can read it
  const signals: Array<{ changedMask: number; baudRate: number }> = [];

  beforeAll(() => {
    if (!addonAvailable) return;
    native = createRequire(import.meta.url)(addonPath) as NativeAddon;
  });

  beforeEach(async () => {
    if (!addonAvailable) return;
    for (const p of native.listPorts()) try { native.destroyPort(p.companionIndex); } catch {}
    await sleep(200);

    const r = native.createPort(0);
    companionIndex = r.companionIndex;
    portNumber     = r.portNumber;

    nativePort   = native.openPort(companionIndex);
    nativeStream = nativePort.createStream();
    companionReceived.length = 0;
    nativeStream.onData(c => { if (c?.length) companionReceived.push(c); });
    nativeStream.onReadError(() => {});
    nativeStream.resumeReading();
    signals.length = 0;
    nativePort.onSignalChange(r => signals.push({ changedMask: r.changedMask, baudRate: r.baudRate }));
    await sleep(100); // let signal watcher issue its first IOCTL

    hCom = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);
    await sleep(100); // let COM_OPEN signal propagate
  });

  afterEach(async () => {
    if (!addonAvailable) return;
    if (hCom) { CloseHandle(hCom); hCom = null; }
    await sleep(50);
    try { nativePort.shutdownSignals(); } catch {}
    try { nativeStream.shutdown(); }      catch {}
    try { nativePort.close(); }           catch {}
    await sleep(50);
    if (companionIndex >= 0) {
      try { native.destroyPort(companionIndex); } catch {}
      companionIndex = -1;
    }
    // Driver Verifier slows teardown; give the driver time to finalize.
    await sleep(500);
  });

  // ════════════════════════════════════════════════════════════════════════
  // Test A — WaitCommEvent (EV_RXCHAR)
  //
  // How PuTTY, Windows Terminal, .NET SerialPort, and most legacy COM
  // applications detect incoming data:
  //   SetCommMask → WaitCommEvent (overlapped, pends) →
  //   data arrives → WaitCommEvent completes with EV_RXCHAR →
  //   ReadFile to get the bytes
  // ════════════════════════════════════════════════════════════════════════

  it("WaitCommEvent: EV_RXCHAR fires when companion writes data", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // 1. Set wait mask = EV_RXCHAR (0x0001)
    const maskBuf = Buffer.alloc(4);
    maskBuf.writeUInt32LE(0x0001, 0);
    DeviceIoControl(hCom, IOCTL_SET_WAIT_MASK, maskBuf, 4, null, 0, Buffer.alloc(4), null);

    // 2. Issue overlapped WaitCommEvent — pends until EV_RXCHAR fires
    const hEvt = CreateEventW(null, true, false, null);
    const ov = mkOv(hEvt);
    const evtResult = Buffer.alloc(4);
    DeviceIoControl(hCom, IOCTL_WAIT_ON_MASK, null, 0, evtResult, 4, Buffer.alloc(4), ov);

    await sleep(100);

    // 3. Companion writes — driver calls GcomCheckWaitMask → EV_RXCHAR fires
    const MSG = Buffer.from("WaitCommEvent!\r\n");
    await new Promise<void>((res, rej) => nativeStream.write(MSG, e => e ? rej(e) : res()));

    // 4. WaitCommEvent should complete now
    const w = WaitForSingleObject(hEvt, 3000);
    CloseHandle(hEvt);
    expect(w).toBe(0); // WAIT_OBJECT_0 — event signaled

    // 5. Read the data
    const data = readOverlapped(hCom, 64, 2000);
    expect(data).not.toBeNull();
    expect(data!.toString()).toContain("WaitCommEvent!");
  });

  // ════════════════════════════════════════════════════════════════════════
  // Test B — Synchronous (non-overlapped) ReadFile
  //
  // How pyserial, simple C/Python scripts, and Arduino IDE read COM ports:
  //   CreateFile(no FILE_FLAG_OVERLAPPED) → ReadFile(null overlapped)
  //
  // We put data in the ring BEFORE issuing ReadFile so the ring is non-empty
  // at call time — this exercises the synchronous completion path in
  // GcomComEvtRead without relying on timeout semantics (which the driver
  // doesn't yet implement for non-MAXDWORD cases).
  // ════════════════════════════════════════════════════════════════════════

  it("synchronous ReadFile: non-overlapped read on COM port returns companion data", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    const MSG = Buffer.from("SyncReadTest\r\n");

    // 1. Write from companion first — data lands in the ring.
    await new Promise<void>((res, rej) => nativeStream.write(MSG, e => e ? rej(e) : res()));
    await sleep(100);

    // 2. Close the overlapped handle from beforeEach (driver: one COM open at a time).
    CloseHandle(hCom!);
    hCom = null;
    await sleep(50);

    // 3. Open COM WITHOUT FILE_FLAG_OVERLAPPED → synchronous I/O mode.
    const hSync = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0, null);
    expect(GetLastError()).toBe(0);

    // 4. Synchronous ReadFile — ring has data, so GcomComEvtRead completes immediately.
    const buf  = Buffer.alloc(64);
    const nBuf = Buffer.alloc(4);
    const ok = ReadFile(hSync, buf, 64, nBuf, null); // null OVERLAPPED = synchronous

    const n = nBuf.readUInt32LE(0);
    CloseHandle(hSync);

    // Reopen overlapped handle so afterEach can close it cleanly
    hCom = CreateFileW(enc16(`\\\\.\\COM${portNumber}`), 0xC0000000, 3, null, 3, 0x40000000, null);

    expect(ok).toBe(true);
    expect(n).toBeGreaterThan(0);
    expect(buf.slice(0, n).toString()).toContain("SyncReadTest");
  });

  // ════════════════════════════════════════════════════════════════════════
  // Test C — GetCommModemStatus (null-modem crossover)
  //
  // How applications read hardware flow-control lines before writing.
  // Via the driver's null-modem crossover:
  //   Companion DTR → COM side sees DSR + DCD
  //   Companion RTS → COM side sees CTS
  // ════════════════════════════════════════════════════════════════════════

  it("GetCommModemStatus: companion DTR/RTS appear as DSR/DCD/CTS on COM side", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // Assert companion signals
    nativePort.setSignals(true, true); // CompDtr=true, CompRts=true
    await sleep(100);

    const status1 = Buffer.alloc(4);
    const ok1 = GetCommModemStatus(hCom, status1);
    expect(ok1).toBe(true);

    const s1 = status1.readUInt32LE(0);
    expect(s1 & SERIAL_DSR_STATE).toBeTruthy(); // CompDtr → DSR
    expect(s1 & SERIAL_DCD_STATE).toBeTruthy(); // CompDtr → DCD
    expect(s1 & SERIAL_CTS_STATE).toBeTruthy(); // CompRts → CTS

    // De-assert and verify signals clear
    nativePort.setSignals(false, false);
    await sleep(100);

    const status2 = Buffer.alloc(4);
    GetCommModemStatus(hCom, status2);
    const s2 = status2.readUInt32LE(0);
    expect(s2 & SERIAL_CTS_STATE).toBe(0);
    expect(s2 & SERIAL_DSR_STATE).toBe(0);
    expect(s2 & SERIAL_DCD_STATE).toBe(0);
  });

  // ════════════════════════════════════════════════════════════════════════
  // Test D — SetBaudRate / GetBaudRate  (subset of SetCommState)
  //
  // Apps like Arduino IDE, pyserial, and PuTTY call SetCommState which
  // internally issues IOCTL_SERIAL_SET_BAUD_RATE + SET_LINE_CONTROL.
  // This test exercises those IOCTLs directly and verifies:
  //   1. the baud rate is stored in the driver's signal state, and
  //   2. the companion's signal watcher receives GCOM_CHANGED_BAUD.
  // ════════════════════════════════════════════════════════════════════════

  it("SetBaudRate/GetBaudRate: baud rate written via IOCTL is stored and readable", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    const GCOM_CHANGED_BAUD = 0x0001;
    // signals[] is populated by the beforeEach onSignalChange handler
    signals.length = 0; // clear any events from port open

    // Issue IOCTL_SERIAL_SET_BAUD_RATE with 115200
    const setBaud = Buffer.alloc(4);
    setBaud.writeUInt32LE(115200, 0);
    const nb1 = Buffer.alloc(4);
    const setOk = DeviceIoControl(hCom, IOCTL_SET_BAUD_RATE, setBaud, 4, null, 0, nb1, null);
    expect(setOk).toBe(true);

    // Verify baud rate was stored by reading back from the companion side.
    // nativePort.getSignals() issues IOCTL_GCOM_GET_SIGNALS on the companion
    // device (overlapped + wait, using our proven sync_ioctl path), which reads
    // pp->SignalState.BaudRate — the same field SET_BAUD_RATE writes to.
    const state = nativePort.getSignals();
    expect(state.baudRate).toBe(115200);

    // Companion should receive GCOM_CHANGED_BAUD signal via the shared watcher
    const deadline = Date.now() + 2000;
    while (Date.now() < deadline) {
      if (signals.some(s => (s.changedMask & GCOM_CHANGED_BAUD) !== 0)) break;
      await sleep(20);
    }
    const baudSignal = signals.find(s => (s.changedMask & GCOM_CHANGED_BAUD) !== 0);
    expect(baudSignal).toBeDefined();
    expect(baudSignal?.baudRate).toBe(115200);
  });

  // ════════════════════════════════════════════════════════════════════════
  // Test E — PurgeComm RXABORT
  //
  // Used by apps that need to cancel in-progress reads before starting a
  // new protocol exchange. RXABORT cancels all IRPs pending in ComReadQueue.
  // ════════════════════════════════════════════════════════════════════════

  it("PurgeComm RXABORT: cancels a pending overlapped read on the COM port", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // 1. Issue an overlapped ReadFile — ring is empty so the IRP pends in ComReadQueue.
    const hEvt = CreateEventW(null, true, false, null);
    const ov = mkOv(hEvt);
    const buf = Buffer.alloc(64);
    ReadFile(hCom, buf, 64, null, ov);
    await sleep(50); // let the IRP land in the manual queue

    // 2. RXABORT: driver calls WdfIoQueuePurge(ComReadQueue) →
    //    the pending IRP is cancelled and its OVERLAPPED event is signaled.
    const flags = Buffer.alloc(4);
    flags.writeUInt32LE(SERIAL_PURGE_RXABORT, 0);
    const purgeOk = DeviceIoControl(hCom, IOCTL_PURGE, flags, 4, null, 0, Buffer.alloc(4), null);
    expect(purgeOk).toBe(true);

    // 3. The cancelled IRP completes within 500ms — WaitForSingleObject returns 0
    //    (WAIT_OBJECT_0 = signaled). A return of 258 would mean timeout (IRP not
    //    cancelled), which would fail the test.
    const w = WaitForSingleObject(hEvt, 500);
    CloseHandle(hEvt);
    expect(w).toBe(0);
  });
});
