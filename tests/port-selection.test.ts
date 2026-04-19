/**
 * Port-number selection tests — exercises the rules for how
 * GhostCOM picks a COM number when creating a virtual port.
 *
 * Three behavioural contracts are verified here:
 *
 *   1. Auto-assign (`createPort(0)`) considers low numbers (COM1-9),
 *      not just COM10+. (The driver used to hard-code COM10 as the
 *      floor; it now scans from COM1 upward.)
 *
 *   2. An explicit `portNumber` works for both low and high numbers,
 *      i.e. the full 1-255 range is reachable.
 *
 *   3. Requesting a number that another (live) port already owns
 *      surfaces a clear, identifiable error — specifically the
 *      HRESULT 0x800700B7 (ERROR_ALREADY_EXISTS) that the TS
 *      wrapper keys its zombie-heal retry off of.
 *
 * Runs against the raw native addon (same pattern as
 * robustness.test.ts) so the driver's collision response reaches
 * us unfiltered by the TS `tryHealZombie` retry.
 *
 * Requires: GhostCOM driver installed, addon built
 * (`bun run build:addon`).
 * Run: bun test tests/port-selection.test.ts
 */

import { describe, it, expect, beforeAll, afterAll, beforeEach } from "bun:test";
import { createRequire } from "node:module";
import path from "node:path";

const addonPath = path.resolve(import.meta.dir, "../addon/ghostcom.node");
let addonAvailable = false;
try {
  createRequire(import.meta.url)(addonPath);
  addonAvailable = true;
} catch { /* addon not built */ }

const SKIP_MSG = "SKIP: addon not built — run `bun run build:addon` first";

interface NativeAddon {
  createPort(n: number): { portNumber: number; companionIndex: number };
  destroyPort(ci: number): void;
  listPorts(): Array<{
    portNumber: number;
    companionIndex: number;
    comSideOpen: boolean;
    companionSideOpen: boolean;
  }>;
}

function sleep(ms: number) {
  return new Promise<void>((r) => setTimeout(r, ms));
}

/**
 * Wipe every port the driver knows about so tests don't leak state
 * into each other. We intentionally call destroy for every entry
 * listPorts returns — the test suite owns the driver for its duration.
 */
async function purgeAllPorts(native: NativeAddon) {
  for (const p of native.listPorts()) {
    try { native.destroyPort(p.companionIndex); } catch { /* ignore */ }
  }
  // Driver finishes symlink teardown asynchronously; give it a moment
  // or subsequent creates race with stale \DosDevices\COM<N> entries.
  await sleep(300);
}

describe("GhostCOM — COM-number selection", () => {
  let native: NativeAddon;

  beforeAll(() => {
    if (!addonAvailable) return;
    native = createRequire(import.meta.url)(addonPath) as NativeAddon;
  });

  // Match the exit pattern used by the other test files — native
  // addon TSFNs can keep bun's loop alive past the last test.
  afterAll(() => { setTimeout(() => process.exit(0), 2000); });

  beforeEach(async () => {
    if (!addonAvailable) return;
    await purgeAllPorts(native);
  });

  // ════════════════════════════════════════════════════════════════════
  // Test 1 — Auto-assign considers low numbers (COM1-9)
  //
  // The driver's GcomFindFreePortNumber starts its scan at 1 and
  // returns the lowest free COM number that isn't already in the
  // NT object namespace. Under `createPort(0)` (auto-assign) the
  // returned number must therefore be low whenever any of COM1-9
  // is free on the host.
  //
  // Strategy: find a concrete free low number N by trying explicit
  // creates from 1..9; this both proves explicit-low works AND gives
  // us a provable upper bound for the auto-assign result. If no low
  // number happens to be free on the test host (unusual — would
  // require nine physical/virtual serial devices already present),
  // the test skips rather than falsely failing.
  // ════════════════════════════════════════════════════════════════════

  it("auto-assign (createPort(0)) hands out a low number when one is free", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // Probe for a known-free low number.
    let knownFreeLow = -1;
    for (let n = 1; n <= 9; n++) {
      try {
        const r = native.createPort(n);
        knownFreeLow = n;
        native.destroyPort(r.companionIndex);
        await sleep(200);
        break;
      } catch { /* taken by the system — try next */ }
    }

    if (knownFreeLow === -1) {
      console.log("SKIP: COM1-9 are all unavailable on this host");
      return;
    }

    // Auto-assign. The driver scans 1..255 lowest-first, so the
    // number we get must be ≤ the concretely-known free slot.
    const auto = native.createPort(0);
    try {
      expect(auto.portNumber).toBeGreaterThanOrEqual(1);
      expect(auto.portNumber).toBeLessThanOrEqual(knownFreeLow);
    } finally {
      native.destroyPort(auto.companionIndex);
      await sleep(200);
    }
  });

  // ════════════════════════════════════════════════════════════════════
  // Test 2 — Explicit portNumber works for both low and high values
  //
  // Exercises the "I know exactly which COM number I want" path for
  // two numbers on opposite ends of the 1-255 range. Both must
  // come back with the exact number we asked for.
  // ════════════════════════════════════════════════════════════════════

  it("explicit portNumber accepts both low (COM3) and high (COM250) values", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // ── Low: COM3 ──
    const LOW = 3;
    let low: { portNumber: number; companionIndex: number } | null = null;
    try {
      low = native.createPort(LOW);
    } catch (e) {
      // If the host has COM3 claimed by something else, skip the
      // low half gracefully rather than mis-reporting a driver bug.
      const msg = (e as Error).message ?? "";
      if (msg.includes("0x800700B7")) {
        console.log(`SKIP low half: COM${LOW} is claimed on this host`);
      } else {
        throw e;
      }
    }
    if (low) {
      try {
        expect(low.portNumber).toBe(LOW);
      } finally {
        native.destroyPort(low.companionIndex);
        await sleep(200);
      }
    }

    // ── High: COM250 ──
    const HIGH = 250;
    let high: { portNumber: number; companionIndex: number } | null = null;
    try {
      high = native.createPort(HIGH);
    } catch (e) {
      const msg = (e as Error).message ?? "";
      if (msg.includes("0x800700B7")) {
        console.log(`SKIP high half: COM${HIGH} is claimed on this host`);
      } else {
        throw e;
      }
    }
    if (high) {
      try {
        expect(high.portNumber).toBe(HIGH);
      } finally {
        native.destroyPort(high.companionIndex);
        await sleep(200);
      }
    }

    // Guard: at least one of the two slots must have worked, otherwise
    // the test provided no signal. On any realistic test host both
    // COM3 and COM250 are free simultaneously — if they aren't, the
    // test environment is unusual enough to warrant visibility.
    expect(low !== null || high !== null).toBe(true);
  });

  // ════════════════════════════════════════════════════════════════════
  // Test 3 — Requesting an in-use port number returns a clear error
  //
  // When the caller asks for a specific COM number that our driver
  // is already holding (live, non-zombie), the second createPort
  // must fail with HRESULT 0x800700B7 — the same signature the TS
  // wrapper's zombie-heal logic keys off of. We explicitly do NOT
  // want a silent success-with-different-number fallback, or a
  // generic/ambiguous error that consumers can't pattern-match.
  // ════════════════════════════════════════════════════════════════════

  it("duplicate explicit portNumber fails with a clear ERROR_ALREADY_EXISTS", async () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }

    // Pick a number that's unlikely to collide with anything on the
    // host. 222 is in the middle of the valid range and well away
    // from both physical ports and typical virtual-COM defaults.
    const TARGET = 222;

    // Create it once — baseline.
    let first: { portNumber: number; companionIndex: number };
    try {
      first = native.createPort(TARGET);
    } catch (e) {
      const msg = (e as Error).message ?? "";
      if (msg.includes("0x800700B7")) {
        console.log(`SKIP: COM${TARGET} is unexpectedly claimed on this host`);
        return;
      }
      throw e;
    }

    try {
      expect(first.portNumber).toBe(TARGET);

      // Second attempt at the same number MUST throw.
      let err: Error | null = null;
      let second: { portNumber: number; companionIndex: number } | null = null;
      try {
        second = native.createPort(TARGET);
      } catch (e) {
        err = e as Error;
      }

      // If by any chance a second handle came back, clean it up so
      // we don't leak driver state into the next test, then fail.
      if (second) {
        try { native.destroyPort(second.companionIndex); } catch {}
      }

      expect(err).not.toBeNull();
      // The error message must carry the HRESULT that the TS wrapper
      // (and any downstream consumer) uses to identify collisions.
      expect(err!.message).toContain("0x800700B7");
    } finally {
      native.destroyPort(first.companionIndex);
      await sleep(200);
    }
  });
});
