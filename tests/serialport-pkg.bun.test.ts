/**
 * serialport-pkg.bun.test.ts — Bun ↔ `serialport` (npm) regression tracker.
 *
 * This suite exercises the virtual COM port using the same `serialport`
 * npm package that real users consume, run under **Bun**. It is the
 * companion to `serialport-pkg.node.test.mjs`, which runs the same
 * surface under Node.js.
 *
 * Why a separate Bun file?
 *   `serialport` uses `ReadFileEx` + APC + `uv_async_send` on its
 *   internal worker thread. Under Bun, `uv_async_send` from a raw
 *   `CreateThread` thread does NOT wake Bun's event loop (see
 *   ISSUES.md § `serialport` + Bun). As a result, every test that
 *   depends on the `'data'` event is expected to FAIL under Bun today.
 *
 *   We keep this file in the suite so that the day Bun fixes the
 *   uv_async_send behaviour, these tests start passing and we notice
 *   immediately — no action required on our part other than "watch
 *   the report".
 *
 * Expected outcome today (Bun 1.3.x):
 *   ✓ "open + close"             passes (CreateFile/CloseHandle path)
 *   ✓ "COM → companion"          passes (serialport writes; companion's
 *                                  own native TSFN delivers data)
 *   ✗ "companion → COM"          FAILS (serialport's 'data' event never
 *                                  fires — the uv_async_send bug)
 *   ✗ "signal round-trip"        FAILS for DTR/RTS readback via
 *                                  serialport callback (same root cause
 *                                  on IOCTL completion via APC).
 *
 * Requires: GhostCOM driver installed, `dist/` built (`bun run build`),
 *           addon built (`bun run build:addon`).
 * Run:      bun test tests/serialport-pkg.bun.test.ts
 */

import {
  describe, it, expect, beforeAll, afterAll, beforeEach, afterEach,
} from "bun:test";
import { createRequire } from "node:module";
import path from "node:path";

// ── Detect dependencies ──────────────────────────────────────────────────────
//
// We intentionally use the compiled public API (`dist/index.js`) rather
// than the raw native addon so the test exercises the exact same entry
// point a real user would hit. Both Bun and Node can require this file.

const distPath   = path.resolve(import.meta.dir, "../dist/index.js");
const addonPath  = path.resolve(import.meta.dir, "../addon/ghostcom.node");
const require_   = createRequire(import.meta.url);

let distAvailable = false;
try { require_(distPath);  distAvailable = true; }  catch { /* not built */ }

let addonAvailable = false;
try { require_(addonPath); addonAvailable = true; } catch { /* not built */ }

let serialportAvailable = false;
try { require_("serialport"); serialportAvailable = true; } catch { /* not installed */ }

const SKIP_MSG =
  !addonAvailable     ? "SKIP: addon not built — run `bun run build:addon`"     :
  !distAvailable      ? "SKIP: dist not built  — run `bun run build:ts`"        :
  !serialportAvailable ? "SKIP: `serialport` not installed — run `bun install`" :
  null;

// ── Imports (deferred through dynamic require so SKIP works) ────────────────

type CreatePortFn = (opts: { portNumber: number }) => Promise<any>;
type ListPortsFn  = () => Array<{ portNumber: number; companionIndex: number }>;
type SerialPortCtor = new (
  opts: { path: string; baudRate: number; autoOpen?: boolean },
  cb?: (err: Error | null) => void,
) => import("stream").Duplex & {
  open(cb: (err: Error | null) => void): void;
  close(cb?: (err: Error | null) => void): void;
  write(chunk: Buffer | string, cb?: (err: Error | null | undefined) => void): boolean;
  set(
    options: { dtr?: boolean; rts?: boolean; brk?: boolean },
    cb?: (err: Error | null) => void,
  ): void;
};

let createPort: CreatePortFn;
let listPorts:  ListPortsFn;
let SerialPort: SerialPortCtor;

// ── Helpers ──────────────────────────────────────────────────────────────────

const sleep = (ms: number) => new Promise<void>(r => setTimeout(r, ms));

/** Race a promise against a timeout. Resolves to `null` on timeout. */
async function withTimeout<T>(p: Promise<T>, ms: number): Promise<T | null> {
  return Promise.race([
    p,
    new Promise<null>(res => setTimeout(() => res(null), ms)),
  ]);
}

// ── Test suite ───────────────────────────────────────────────────────────────

describe("serialport (npm) — Bun regression tracker", () => {
  let port: any;                 // GhostCOM VirtualPort (companion)
  let sp: InstanceType<SerialPortCtor> | null = null;
  let portNumber = -1;

  // ── Suite setup ──────────────────────────────────────────────────────

  beforeAll(() => {
    if (SKIP_MSG) return;
    const dist = require_(distPath) as {
      createPort: CreatePortFn; listPorts: ListPortsFn;
    };
    createPort = dist.createPort;
    listPorts  = dist.listPorts;
    SerialPort = (require_("serialport") as { SerialPort: SerialPortCtor }).SerialPort;
  });

  // Force-exit after suite completes — our native addon TSFNs keep Bun's
  // event loop alive even after destroy(). 2 s lets the JUnit reporter flush.
  afterAll(() => { setTimeout(() => process.exit(0), 2000); });

  // ── Per-test setup ───────────────────────────────────────────────────

  beforeEach(async () => {
    if (SKIP_MSG) return;

    // Clean any leftovers from an earlier failed run.
    for (const p of listPorts()) {
      try { (require_(addonPath) as any).destroyPort(p.companionIndex); }
      catch { /* ignore */ }
    }
    await sleep(200);

    // Create virtual port (auto-assign number).
    port = await createPort({ portNumber: 0 });
    portNumber = port.portNumber;

    // Assert companion DTR+RTS so the COM side appears as a "connected
    // modem" (DSR+DCD+CTS) — serialport is happier with this.
    port.setSignals({ dtr: true, rts: true });

    await sleep(100);

    // Open serialport on the COM side.
    sp = new SerialPort({
      path: `COM${portNumber}`,
      baudRate: 115200,
      autoOpen: false,
    });
    await new Promise<void>((res, rej) =>
      sp!.open(err => (err ? rej(err) : res())),
    );

    // Let COM_OPEN signal propagate.
    await sleep(150);
  });

  // ── Per-test teardown ────────────────────────────────────────────────

  afterEach(async () => {
    if (SKIP_MSG) return;

    if (sp) {
      // serialport.close() can hang in Bun if the read thread is stuck
      // on SleepEx waiting for an APC; race with a timeout to avoid
      // blocking the suite.
      await withTimeout(
        new Promise<void>((res, rej) =>
          sp!.close(err => (err ? rej(err) : res())),
        ),
        2000,
      );
      sp = null;
    }

    try { await port?.destroy(); } catch { /* ignore */ }
    port = null;

    // Driver Verifier slows teardown — give it a beat.
    await sleep(500);
  });

  // ════════════════════════════════════════════════════════════════════
  // 1. Baseline — opening and closing via serialport succeeds.
  //    Works today in both Bun and Node.
  // ════════════════════════════════════════════════════════════════════

  it("baseline: serialport opens the virtual COM port without error", () => {
    if (SKIP_MSG) { console.log(SKIP_MSG); return; }
    expect(sp).not.toBeNull();
    // If open() had failed, beforeEach would have thrown already.
  });

  // ════════════════════════════════════════════════════════════════════
  // 2. COM → companion: serialport WRITES, companion READS.
  //    The read path on the companion side uses our own NAPI TSFN,
  //    which works correctly in Bun. Expected to PASS in Bun today.
  // ════════════════════════════════════════════════════════════════════

  it("COM → companion: serialport.write() delivers bytes to the companion stream", async () => {
    if (SKIP_MSG) { console.log(SKIP_MSG); return; }

    const MSG = "Pong from serialport!\r\n";

    const received: Buffer[] = [];
    port.stream.on("data", (chunk: Buffer) => received.push(chunk));

    await new Promise<void>((res, rej) =>
      sp!.write(MSG, err => (err ? rej(err) : res())),
    );

    const deadline = Date.now() + 3000;
    while (
      Date.now() < deadline &&
      !Buffer.concat(received).toString().includes(MSG.trim())
    ) {
      await sleep(20);
    }

    expect(Buffer.concat(received).toString()).toContain(MSG.trim());
  });

  // ════════════════════════════════════════════════════════════════════
  // 3. companion → COM: companion WRITES, serialport READS.
  //    This is the Bun-regression canary — serialport's ReadFileEx
  //    APC fires at the OS level, but the uv_async_send that would
  //    notify Bun's JS side does nothing. Expected to FAIL in Bun
  //    today; will start passing the day Bun fixes uv_async_send
  //    from CreateThread threads.
  // ════════════════════════════════════════════════════════════════════

  it("companion → COM: companion.write() fires a 'data' event on serialport  [expected FAIL in Bun]", async () => {
    if (SKIP_MSG) { console.log(SKIP_MSG); return; }

    const MSG = "Ping from companion!\r\n";

    const received: Buffer[] = [];
    sp!.on("data", (chunk: Buffer) => received.push(chunk));

    await new Promise<void>((res, rej) =>
      port.stream.write(Buffer.from(MSG), (err: Error | null | undefined) =>
        err ? rej(err) : res(),
      ),
    );

    // Poll up to 5 s. Today (Bun 1.3.x) the 'data' event never fires,
    // so this loop will run to the deadline and the assertion below
    // will fail with "expected '' to contain '...'".
    const deadline = Date.now() + 5000;
    while (
      Date.now() < deadline &&
      !Buffer.concat(received).toString().includes(MSG.trim())
    ) {
      await sleep(50);
    }

    expect(Buffer.concat(received).toString()).toContain(MSG.trim());
  }, 10_000);

  // ════════════════════════════════════════════════════════════════════
  // 4. Signal round-trip: serialport.set({dtr}) triggers companion
  //    signal event. The companion side uses our native TSFN, so the
  //    companion 'signal' event fires even in Bun.
  // ════════════════════════════════════════════════════════════════════

  it("signal: serialport.set({ dtr, rts }) propagates to companion 'signal' event", async () => {
    if (SKIP_MSG) { console.log(SKIP_MSG); return; }

    const changes: any[] = [];
    port.on("signal", (s: any) => changes.push(s));

    // Toggle DTR off then on.
    await new Promise<void>((res, rej) =>
      sp!.set({ dtr: false, rts: false }, e => (e ? rej(e) : res())),
    );
    await sleep(150);

    await new Promise<void>((res, rej) =>
      sp!.set({ dtr: true, rts: true }, e => (e ? rej(e) : res())),
    );
    await sleep(150);

    // At least one signal event should have been observed.
    expect(changes.length).toBeGreaterThan(0);
  });
});
