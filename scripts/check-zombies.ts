#!/usr/bin/env bun
/**
 * check-zombies.ts — find (and optionally destroy) orphaned GhostCOM ports.
 *
 * A "zombie" port is one the driver still knows about but no process
 * has the companion handle open. This happens when the owning process
 * dies abnormally — SIGKILL, uncaught exception, TaskManager kill,
 * `TaskStop`, BSOD — before `VirtualPort.destroy()` runs and fires
 * IOCTL_GCOM_DESTROY_PORT.
 *
 * The driver's current lifecycle model destroys port pairs only on an
 * explicit control-device IOCTL, not on last-handle-close, so any
 * crash before that IOCTL leaves a stale entry in SERIALCOMM and a
 * dangling `\DosDevices\COM<N>` symlink. See ISSUES.md "Port leaks on
 * abnormal termination".
 *
 * Usage:
 *   bun run scripts/check-zombies.ts              # report only, exit 1 if zombies found
 *   bun run scripts/check-zombies.ts --clean      # destroy any zombies found
 *   bun run scripts/check-zombies.ts --json       # machine-readable output
 *   bun run scripts/check-zombies.ts --clean --json
 *
 * Exit codes:
 *   0 — no zombies found (or --clean succeeded on all)
 *   1 — zombies present and not cleaned (or clean partially failed)
 *   2 — driver not installed / not reachable
 */

import {
  nativeListPorts,
  nativeDestroyPort,
  nativeIsDriverAvailable,
} from "../src/control.js";
import type { PortInfo } from "../src/types.js";

// --- argument parsing -----------------------------------------------------

const args = new Set(process.argv.slice(2));
const clean = args.has("--clean");
const asJson = args.has("--json");

// --- driver preflight -----------------------------------------------------

if (!nativeIsDriverAvailable()) {
  if (asJson) {
    console.log(
      JSON.stringify({
        ok: false,
        error: "driver-not-available",
        ports: [],
        zombies: [],
      }),
    );
  } else {
    console.error("GhostCOM driver is not installed or not reachable.");
    console.error("Install with: bun run install:driver");
  }
  process.exit(2);
}

// --- scan -----------------------------------------------------------------

/**
 * A port is a zombie when the companion side — the one held by the
 * creating Node/Bun process — is closed. `comSideOpen` reflects whether
 * an external terminal is currently connected and is irrelevant: a live
 * server with no client is not a leak, and a dead server with a client
 * can't exist (the driver would have refused to service the client).
 */
function isZombie(p: PortInfo): boolean {
  return !p.companionSideOpen;
}

const ports = nativeListPorts();
const zombies = ports.filter(isZombie);

// --- clean ----------------------------------------------------------------

interface CleanResult {
  port: PortInfo;
  destroyed: boolean;
  error?: string;
}

const cleanResults: CleanResult[] = [];

if (clean) {
  for (const z of zombies) {
    try {
      nativeDestroyPort(z.companionIndex);
      cleanResults.push({ port: z, destroyed: true });
    } catch (err) {
      cleanResults.push({
        port: z,
        destroyed: false,
        error: (err as Error).message,
      });
    }
  }
}

// --- report ---------------------------------------------------------------

if (asJson) {
  console.log(
    JSON.stringify(
      {
        ok: true,
        cleaned: clean,
        ports,
        zombies,
        cleanResults: clean ? cleanResults : undefined,
      },
      null,
      2,
    ),
  );
} else {
  if (ports.length === 0) {
    console.log("No GhostCOM ports in the driver's table.");
  } else {
    console.log(`${ports.length} GhostCOM port(s) known to the driver:`);
    for (const p of ports) {
      const tag = isZombie(p) ? "ZOMBIE" : "live  ";
      console.log(
        `  [${tag}] COM${p.portNumber.toString().padEnd(3)}  ` +
          `companion=${p.companionIndex}  ` +
          `com=${p.comSideOpen ? "open" : "closed"}  ` +
          `companion=${p.companionSideOpen ? "open" : "closed"}`,
      );
    }
  }

  if (clean && cleanResults.length > 0) {
    console.log();
    const destroyed = cleanResults.filter((r) => r.destroyed).length;
    console.log(`Cleanup: destroyed ${destroyed}/${cleanResults.length}`);
    for (const r of cleanResults) {
      if (!r.destroyed) {
        console.log(
          `  ! COM${r.port.portNumber} (companion=${r.port.companionIndex}): ${r.error}`,
        );
      }
    }
  } else if (zombies.length > 0) {
    console.log();
    console.log(
      `Found ${zombies.length} zombie port(s). Re-run with --clean to destroy.`,
    );
  }
}

// --- exit code ------------------------------------------------------------

if (clean) {
  const failed = cleanResults.some((r) => !r.destroyed);
  process.exit(failed ? 1 : 0);
} else {
  process.exit(zombies.length > 0 ? 1 : 0);
}
