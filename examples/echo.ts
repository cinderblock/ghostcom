/**
 * echo.ts — Simple echo server.
 *
 * Creates a virtual COM port and echoes all received data back to
 * the sender. Open the port in a serial terminal (PuTTY, Tera Term,
 * etc.) and type — your characters will be echoed back.
 *
 * Usage:
 *   bun run examples/echo.ts [port-number]
 */

import { createPort, SignalChanged } from "../src/index.js";

const portNumber = parseInt(process.argv[2] ?? "10", 10);

console.log(`Creating virtual COM port: COM${portNumber}...`);

const port = await createPort({ portNumber });
console.log(`✓ ${port.portName} is ready`);
console.log(`  Open ${port.portName} in a serial terminal to test`);
console.log();

port.on("com-open", () => {
  console.log("→ External application connected");
  port.setSignals({ dtr: true, rts: true });
});

port.on("com-close", () => {
  console.log("← External application disconnected");
});

port.on("signal", (state) => {
  const changes: string[] = [];
  if (state.changedMask & SignalChanged.BAUD) {
    changes.push(`baud=${state.baudRate}`);
  }
  if (state.changedMask & SignalChanged.LINE_CONTROL) {
    changes.push(`${state.dataBits}${state.parity[0]!.toUpperCase()}${state.stopBits === "one" ? "1" : "2"}`);
  }
  if (state.changedMask & SignalChanged.DTR) {
    changes.push(`DTR=${state.dtr ? "ON" : "OFF"}`);
  }
  if (state.changedMask & SignalChanged.RTS) {
    changes.push(`RTS=${state.rts ? "ON" : "OFF"}`);
  }
  if (changes.length > 0) {
    console.log(`  signal: ${changes.join(", ")}`);
  }
});

// Echo loop
port.stream.on("data", (chunk: Buffer) => {
  const hex = chunk.toString("hex").match(/.{1,2}/g)?.join(" ");
  console.log(`  rx ${chunk.length} bytes: ${hex}`);
  port.stream.write(chunk);
});

port.stream.on("error", (err) => {
  console.error("Stream error:", err.message);
});

// Graceful shutdown
const shutdown = async () => {
  console.log("\nShutting down...");
  await port.destroy();
  console.log("Done.");
  process.exit(0);
};

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

console.log("Press Ctrl+C to stop");
