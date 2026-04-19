/**
 * loopback.ts — Virtual loopback COM port.
 *
 * Creates a virtual COM port that echoes every byte received straight
 * back to the sender — a pure software loopback. Open it in PuTTY with
 * local echo OFF and type; each keystroke travels out, loops back, and
 * appears on your screen.
 *
 * With --signals / -s, human-readable serial control-signal messages
 * (baud rate, DTR/RTS, flow control, etc.) are injected inline into the
 * loopback stream so they show up in your terminal alongside your typing.
 *
 * Usage:
 *   bun run examples/loopback.ts [port-number] [--signals|-s]
 *
 * Examples:
 *   bun run examples/loopback.ts            # COM11, plain loopback
 *   bun run examples/loopback.ts 12         # COM12, plain loopback
 *   bun run examples/loopback.ts 12 -s      # COM12, inline signals
 *
 * PuTTY setup tips:
 *   • Connection → Serial → Serial line: COM<N>
 *   • Terminal → "Implicit LF in every CR: ON"   (Enter renders a newline)
 *   • Terminal → "Local echo: Force off"
 *   • Terminal → "Local line editing: Force off"
 */

import { createPort, SignalChanged, type SignalState } from "../src/index.js";

// --- argument parsing -----------------------------------------------------

const args = process.argv.slice(2);
const printSignals = args.some((a) => a === "--signals" || a === "-s");
const positional = args.find((a) => !a.startsWith("-"));
const portNumber = parseInt(positional ?? "11", 10);

if (!Number.isFinite(portNumber) || portNumber < 1 || portNumber > 255) {
  console.error(`Invalid port number: ${positional ?? "(missing)"}`);
  process.exit(1);
}

// --- create the port ------------------------------------------------------

console.log(`Creating virtual COM port: COM${portNumber}...`);
const port = await createPort({ portNumber });
console.log(
  `✓ ${port.portName} ready — loopback mode${printSignals ? " + inline signals" : ""}`,
);
console.log(`  Open in PuTTY: Connection → Serial → ${port.portName}`);
console.log(
  `  Terminal: "Implicit LF in every CR" ON; Local echo & line editing: Force off`,
);
console.log(`  Ctrl+C to stop`);
console.log();

// Assert companion DTR+RTS so the COM side sees DSR+DCD+CTS — "modem connected".
port.setSignals({ dtr: true, rts: true });

// --- loopback -------------------------------------------------------------

port.stream.on("data", (chunk: Buffer) => {
  port.stream.write(chunk);
});

port.stream.on("error", (err) => {
  console.error("Stream error:", err.message);
});

// --- signal handling ------------------------------------------------------

function writeInline(msg: string): void {
  // CRLF-wrap so the message lands on its own line even mid-typing.
  port.stream.write(Buffer.from(`\r\n[${msg}]\r\n`, "utf8"));
}

function describeSignal(state: SignalState): string {
  const mask = state.changedMask;
  const out: string[] = [];
  if (mask & SignalChanged.BAUD) out.push(`baud=${state.baudRate}`);
  if (mask & SignalChanged.LINE_CONTROL) {
    const p = state.parity[0]!.toUpperCase();
    const s =
      state.stopBits === "one" ? "1" : state.stopBits === "two" ? "2" : "1.5";
    out.push(`${state.dataBits}${p}${s}`);
  }
  if (mask & SignalChanged.DTR) out.push(`DTR=${state.dtr ? "on" : "off"}`);
  if (mask & SignalChanged.RTS) out.push(`RTS=${state.rts ? "on" : "off"}`);
  if (mask & SignalChanged.BREAK) {
    out.push(`BREAK=${state.breakState ? "on" : "off"}`);
  }
  if (mask & SignalChanged.HANDFLOW) {
    out.push(
      `flow=0x${state.handflow.controlHandshake.toString(16)}` +
        `/0x${state.handflow.flowReplace.toString(16)}`,
    );
  }
  if (mask & SignalChanged.CHARS) out.push("chars");
  if (mask & SignalChanged.WAIT_MASK) {
    out.push(`waitmask=0x${state.waitMask.toString(16)}`);
  }
  return out.join(" ");
}

port.on("com-open", () => {
  console.log("→ External application connected");
  if (printSignals) writeInline("connected");
});

port.on("com-close", () => {
  console.log("← External application disconnected");
});

port.on("signal", (state) => {
  const summary = describeSignal(state);
  if (!summary) return; // pure COM_OPEN/COM_CLOSE events — handled above
  console.log(`  signal: ${summary}`);
  if (printSignals && port.isComOpen) writeInline(`signal ${summary}`);
});

// --- shutdown -------------------------------------------------------------
//
// The driver destroys port pairs only on an explicit IOCTL, never on
// last-handle-close, so any exit path that skips `port.destroy()`
// leaves a zombie. We cover every reachable path here. SIGKILL and
// BSOD are unreachable; `createPort()`'s zombie auto-heal cleans those
// up on the next run.

let shuttingDown = false;

const shutdown = async (code = 0) => {
  if (shuttingDown) return;
  shuttingDown = true;
  console.log("\nShutting down...");
  try {
    await port.destroy();
  } catch (err) {
    console.error("Destroy error:", (err as Error).message);
  }
  process.exit(code);
};

process.on("SIGINT", () => shutdown(0));
process.on("SIGTERM", () => shutdown(0));

process.on("uncaughtException", (err) => {
  console.error("Uncaught exception:", err);
  void shutdown(1);
});
process.on("unhandledRejection", (reason) => {
  console.error("Unhandled rejection:", reason);
  void shutdown(1);
});
