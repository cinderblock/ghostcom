/**
 * pair.ts — Create two virtual COM ports and bridge them together.
 *
 * This creates a virtual null-modem cable: anything sent to one port
 * appears on the other, and vice versa. Useful for testing serial
 * applications without physical hardware.
 *
 * Usage:
 *   bun run examples/pair.ts [port-a] [port-b]
 */

import { createPort } from "../src/index.js";

const portA = parseInt(process.argv[2] ?? "10", 10);
const portB = parseInt(process.argv[3] ?? "11", 10);

console.log(`Creating virtual null-modem pair: COM${portA} ↔ COM${portB}...`);

const [a, b] = await Promise.all([
  createPort({ portNumber: portA }),
  createPort({ portNumber: portB }),
]);

console.log(`✓ ${a.portName} ↔ ${b.portName} ready`);
console.log();

// Bridge data in both directions
a.stream.pipe(b.stream);
b.stream.pipe(a.stream);

// Assert carrier signals
a.setSignals({ dtr: true, rts: true });
b.setSignals({ dtr: true, rts: true });

// Log data flow
let bytesAtoB = 0;
let bytesBtoA = 0;

a.stream.on("data", (chunk: Buffer) => {
  bytesBtoA += chunk.length;
});

b.stream.on("data", (chunk: Buffer) => {
  bytesAtoB += chunk.length;
});

// Status line
const statusInterval = setInterval(() => {
  process.stdout.write(
    `\r  ${a.portName}→${b.portName}: ${bytesAtoB} bytes  |  ` +
      `${b.portName}→${a.portName}: ${bytesBtoA} bytes`,
  );
}, 500);

// Events
a.on("com-open", () => console.log(`\n→ ${a.portName} opened`));
a.on("com-close", () => console.log(`\n← ${a.portName} closed`));
b.on("com-open", () => console.log(`\n→ ${b.portName} opened`));
b.on("com-close", () => console.log(`\n← ${b.portName} closed`));

const shutdown = async () => {
  clearInterval(statusInterval);
  console.log("\n\nShutting down...");
  await Promise.all([a.destroy(), b.destroy()]);
  console.log("Done.");
  process.exit(0);
};

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

console.log("Press Ctrl+C to stop");
