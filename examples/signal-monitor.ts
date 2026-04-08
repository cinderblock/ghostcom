/**
 * signal-monitor.ts — Monitor serial signal changes on a virtual port.
 *
 * Creates a virtual COM port and logs every serial configuration
 * change made by the external application. Useful for debugging
 * and understanding how applications configure serial ports.
 *
 * Usage:
 *   bun run examples/signal-monitor.ts [port-number]
 */

import { createPort, SignalChanged, type SignalState } from "../src/index.js";

const portNumber = parseInt(process.argv[2] ?? "20", 10);

console.log(`Creating virtual COM port: COM${portNumber}...`);

const port = await createPort({ portNumber });
console.log(`✓ ${port.portName} is ready — monitoring signals`);
console.log();

function formatSignalChange(state: SignalState): void {
  const mask = state.changedMask;
  const seq = state.sequenceNumber.toString().padStart(4, " ");

  if (mask & SignalChanged.COM_OPEN) {
    console.log(`[${seq}] ▶ COM port OPENED by external application`);
  }

  if (mask & SignalChanged.COM_CLOSE) {
    console.log(`[${seq}] ◼ COM port CLOSED`);
  }

  if (mask & SignalChanged.BAUD) {
    console.log(`[${seq}] Baud rate: ${state.baudRate}`);
  }

  if (mask & SignalChanged.LINE_CONTROL) {
    console.log(
      `[${seq}] Line control: ${state.dataBits} data bits, ` +
        `${state.parity} parity, ${state.stopBits} stop bits`,
    );
  }

  if (mask & SignalChanged.DTR) {
    console.log(`[${seq}] DTR: ${state.dtr ? "ASSERTED" : "CLEARED"}`);
  }

  if (mask & SignalChanged.RTS) {
    console.log(`[${seq}] RTS: ${state.rts ? "ASSERTED" : "CLEARED"}`);
  }

  if (mask & SignalChanged.BREAK) {
    console.log(
      `[${seq}] BREAK: ${state.breakState ? "ON" : "OFF"}`,
    );
  }

  if (mask & SignalChanged.HANDFLOW) {
    console.log(
      `[${seq}] Flow control: handshake=0x${state.handflow.controlHandshake.toString(16)}, ` +
        `replace=0x${state.handflow.flowReplace.toString(16)}, ` +
        `xonLimit=${state.handflow.xonLimit}, xoffLimit=${state.handflow.xoffLimit}`,
    );
  }

  if (mask & SignalChanged.CHARS) {
    console.log(
      `[${seq}] Special chars: XON=0x${state.specialChars.xonChar.toString(16)}, ` +
        `XOFF=0x${state.specialChars.xoffChar.toString(16)}, ` +
        `EOF=0x${state.specialChars.eofChar.toString(16)}`,
    );
  }

  if (mask & SignalChanged.WAIT_MASK) {
    console.log(`[${seq}] Wait mask: 0x${state.waitMask.toString(16)}`);
  }
}

port.on("signal", formatSignalChange);

// Assert companion signals so the COM side sees a "connected" modem
port.setSignals({ dtr: true, rts: true });

// Silently consume any data (don't let the ring buffer fill)
port.stream.resume();

const shutdown = async () => {
  console.log("\nShutting down...");
  await port.destroy();
  process.exit(0);
};

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

console.log("Press Ctrl+C to stop");
