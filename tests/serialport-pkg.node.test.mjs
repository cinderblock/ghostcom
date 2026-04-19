/**
 * serialport-pkg.node.test.mjs — Node.js ↔ `serialport` (npm).
 *
 * Runs the same virtual-COM surface as `serialport-pkg.bun.test.ts`,
 * but under Node.js using the built-in `node:test` runner. This is
 * where we expect the `serialport` package to actually work end-to-end
 * — Node doesn't have the uv_async_send-from-CreateThread regression
 * that breaks serialport under Bun (see ISSUES.md).
 *
 * All four tests below are expected to pass once the driver has been
 * rebuilt with the `ReadIntervalTimeout == MAXDWORD` fix in
 * `driver/src/comport.c` (see ISSUES.md). Until that rebuild lands,
 * "companion → COM" will still fail — but with a different root cause
 * than the Bun case: serialport's drain-ReadFile stays pended instead
 * of returning 0 bytes immediately.
 *
 * Runtime: requires Node 20.13+ for the `--test-reporter=junit` stable
 * output. On older Node, remove the JUnit flags — plain output still
 * works fine on 18+.
 *
 * Requires: GhostCOM driver installed, `dist/` built (`bun run build`),
 *           addon built (`bun run build:addon`).
 * Run:      node --test tests/serialport-pkg.node.test.mjs
 *           (the harness runner — scripts/run-tests.ts — invokes it
 *            automatically when Node.js is available on PATH.)
 */

import { describe, it, before, after, beforeEach, afterEach } from "node:test";
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import path from "node:path";
import { fileURLToPath } from "node:url";

// ── Detect dependencies ──────────────────────────────────────────────────────

const __dirname  = path.dirname(fileURLToPath(import.meta.url));
const distPath   = path.resolve(__dirname, "../dist/index.js");
const addonPath  = path.resolve(__dirname, "../addon/ghostcom.node");
const require_   = createRequire(import.meta.url);

let distAvailable = false;
try { require_(distPath);  distAvailable = true; }  catch { /* not built */ }

let addonAvailable = false;
try { require_(addonPath); addonAvailable = true; } catch { /* not built */ }

let serialportAvailable = false;
try { require_("serialport"); serialportAvailable = true; } catch { /* not installed */ }

const SKIP_MSG =
  !addonAvailable      ? "SKIP: addon not built — run `bun run build:addon`"      :
  !distAvailable       ? "SKIP: dist not built  — run `bun run build:ts`"         :
  !serialportAvailable ? "SKIP: `serialport` not installed — run `bun install`"   :
  null;

// ── Helpers ──────────────────────────────────────────────────────────────────

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function withTimeout(p, ms) {
  return Promise.race([
    p,
    new Promise((res) => setTimeout(() => res(null), ms)),
  ]);
}

// ── Test suite ───────────────────────────────────────────────────────────────

describe("serialport (npm) — Node.js end-to-end", { concurrency: false }, () => {
  /** @type {any} */ let createPort;
  /** @type {any} */ let listPorts;
  /** @type {any} */ let SerialPort;

  /** @type {any} */ let port = null;      // companion side
  /** @type {any} */ let sp   = null;      // COM side
  let portNumber = -1;

  // ── Suite setup ──────────────────────────────────────────────────────

  before(() => {
    if (SKIP_MSG) return;
    const dist = require_(distPath);
    createPort = dist.createPort;
    listPorts  = dist.listPorts;
    SerialPort = require_("serialport").SerialPort;
  });

  // Force-exit after suite completes — the native addon's TSFN threads
  // keep Node's event loop alive even after destroy(). Give the JUnit
  // reporter 2 s to flush, then exit.
  after(() => {
    setTimeout(() => process.exit(0), 2000).unref?.();
  });

  // ── Per-test setup ───────────────────────────────────────────────────

  beforeEach(async () => {
    if (SKIP_MSG) return;

    // Clean any leftovers from an earlier failed run.
    for (const p of listPorts()) {
      try { require_(addonPath).destroyPort(p.companionIndex); }
      catch { /* ignore */ }
    }
    await sleep(200);

    port = await createPort({ portNumber: 0 });
    portNumber = port.portNumber;
    port.setSignals({ dtr: true, rts: true });
    await sleep(100);

    sp = new SerialPort({
      path: `COM${portNumber}`,
      baudRate: 115200,
      autoOpen: false,
    });
    await new Promise((res, rej) =>
      sp.open((err) => (err ? rej(err) : res())),
    );
    await sleep(150);
  });

  // ── Per-test teardown ────────────────────────────────────────────────

  afterEach(async () => {
    if (SKIP_MSG) return;

    if (sp) {
      await withTimeout(
        new Promise((res, rej) =>
          sp.close((err) => (err ? rej(err) : res())),
        ),
        2000,
      );
      sp = null;
    }

    try { await port?.destroy(); } catch { /* ignore */ }
    port = null;

    await sleep(500);
  });

  // ════════════════════════════════════════════════════════════════════
  // 1. Baseline open
  // ════════════════════════════════════════════════════════════════════

  it("baseline: serialport opens the virtual COM port without error", (t) => {
    if (SKIP_MSG) { t.skip(SKIP_MSG); return; }
    assert.ok(sp, "SerialPort instance was not created");
  });

  // ════════════════════════════════════════════════════════════════════
  // 2. COM → companion (serialport writes, companion reads)
  // ════════════════════════════════════════════════════════════════════

  it("COM → companion: serialport.write() delivers bytes to the companion stream", async (t) => {
    if (SKIP_MSG) { t.skip(SKIP_MSG); return; }

    const MSG = "Pong from serialport!\r\n";

    /** @type {Buffer[]} */ const received = [];
    port.stream.on("data", (chunk) => received.push(chunk));

    await new Promise((res, rej) =>
      sp.write(MSG, (err) => (err ? rej(err) : res())),
    );

    const deadline = Date.now() + 3000;
    while (
      Date.now() < deadline &&
      !Buffer.concat(received).toString().includes(MSG.trim())
    ) {
      await sleep(20);
    }

    assert.ok(
      Buffer.concat(received).toString().includes(MSG.trim()),
      `expected companion to receive ${JSON.stringify(MSG.trim())}; got ${JSON.stringify(Buffer.concat(received).toString())}`,
    );
  });

  // ════════════════════════════════════════════════════════════════════
  // 3. companion → COM (companion writes, serialport reads)
  //    This is the real-world serialport path that matters most. Under
  //    Node it is expected to PASS (given the driver rebuild); under
  //    Bun it is the canary that lives in the .bun.test.ts file.
  // ════════════════════════════════════════════════════════════════════

  it("companion → COM: companion.write() fires a 'data' event on serialport", async (t) => {
    if (SKIP_MSG) { t.skip(SKIP_MSG); return; }

    const MSG = "Ping from companion!\r\n";

    /** @type {Buffer[]} */ const received = [];
    sp.on("data", (chunk) => received.push(chunk));

    await new Promise((res, rej) =>
      port.stream.write(Buffer.from(MSG), (err) => (err ? rej(err) : res())),
    );

    const deadline = Date.now() + 5000;
    while (
      Date.now() < deadline &&
      !Buffer.concat(received).toString().includes(MSG.trim())
    ) {
      await sleep(50);
    }

    assert.ok(
      Buffer.concat(received).toString().includes(MSG.trim()),
      `expected serialport to receive ${JSON.stringify(MSG.trim())}; got ${JSON.stringify(Buffer.concat(received).toString())}`,
    );
  });

  // ════════════════════════════════════════════════════════════════════
  // 4. Signal round-trip: serialport toggles DTR/RTS, companion observes.
  // ════════════════════════════════════════════════════════════════════

  it("signal: serialport.set({ dtr, rts }) propagates to companion 'signal' event", async (t) => {
    if (SKIP_MSG) { t.skip(SKIP_MSG); return; }

    /** @type {any[]} */ const changes = [];
    port.on("signal", (s) => changes.push(s));

    await new Promise((res, rej) =>
      sp.set({ dtr: false, rts: false }, (e) => (e ? rej(e) : res())),
    );
    await sleep(150);

    await new Promise((res, rej) =>
      sp.set({ dtr: true, rts: true }, (e) => (e ? rej(e) : res())),
    );
    await sleep(150);

    assert.ok(
      changes.length > 0,
      "expected at least one 'signal' event on the companion",
    );
  });
});
