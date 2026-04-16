/**
 * PnP enumeration tests — verifies the COM port is discoverable by
 * standard Windows serial-port enumeration mechanisms.
 *
 * STATUS: expected to FAIL with the current control-device architecture.
 * These tests document the gap between what the driver exposes (just a
 * `\DosDevices\COM<N>` symlink, openable via CreateFile) and what real
 * serial-port consumers actually need (a child PDO in the Ports class
 * registered with GUID_DEVINTERFACE_COMPORT).
 *
 * See ISSUES.md "createPort succeeds in JS but no COM child PDO appears
 * in Device Manager".
 */

import { describe, it, expect, beforeAll, beforeEach, afterEach } from "bun:test";
import { createRequire } from "node:module";
import { spawnSync } from "node:child_process";
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
  openPort(ci: number): any;
  destroyPort(ci: number): void;
  listPorts(): Array<{ portNumber: number; companionIndex: number }>;
}

function sleep(ms: number) { return new Promise<void>(r => setTimeout(r, ms)); }

/** Invoke PowerShell and return stdout trimmed. */
function ps(script: string): { stdout: string; stderr: string; exitCode: number } {
  const r = spawnSync(
    "C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
    ["-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
    { encoding: "utf8" },
  );
  return {
    stdout: (r.stdout ?? "").trim(),
    stderr: (r.stderr ?? "").trim(),
    exitCode: r.status ?? -1,
  };
}

describe("GhostCOM — Windows PnP enumeration (expected failures until PDO rewrite)", () => {
  let native: NativeAddon;
  let companionIndex = -1;
  let portNumber = -1;
  let nativePort: any;
  let nativeStream: any;

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

    const r = native.createPort(0);
    portNumber = r.portNumber;
    companionIndex = r.companionIndex;
    nativePort = native.openPort(companionIndex);
    nativeStream = nativePort.createStream();
    nativeStream.onData(() => {});
    nativeStream.onReadError(() => {});
    nativeStream.resumeReading();
    nativePort.onSignalChange(() => {});
    await sleep(500);
  });

  afterEach(async () => {
    if (!addonAvailable) return;
    try { nativePort.shutdownSignals(); } catch {}
    try { nativeStream.shutdown(); }      catch {}
    try { nativePort.close(); }           catch {}
    await sleep(50);
    if (companionIndex >= 0) {
      try { native.destroyPort(companionIndex); } catch {}
      companionIndex = -1;
    }
    await sleep(500);
  });

  it("HKLM\\HARDWARE\\DEVICEMAP\\SERIALCOMM contains our port", () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const r = ps(`
      Get-ItemProperty "HKLM:\\HARDWARE\\DEVICEMAP\\SERIALCOMM" |
        Select-Object -ExpandProperty "COM${portNumber}" -ErrorAction SilentlyContinue |
        Out-String
    `);
    expect(r.stdout.length).toBeGreaterThan(0);
  });

  it("Get-PnpDevice -Class Ports lists our COM port", () => {
    if (!addonAvailable) { console.log(SKIP_MSG); return; }
    const r = ps(`
      Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -match "\\(COM${portNumber}\\)" } |
        Select-Object -ExpandProperty FriendlyName |
        Out-String
    `);
    expect(r.stdout).toMatch(new RegExp(`\\(COM${portNumber}\\)`));
  });
});
