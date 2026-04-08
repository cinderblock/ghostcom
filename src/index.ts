/**
 * # node-null
 *
 * Virtual COM port creation for Node.js and Bun on Windows.
 *
 * Create fake serial ports that appear as real COM ports to the
 * operating system. External applications can open and use them as
 * if they were physical serial devices, while your Node.js/Bun code
 * controls the other end through a high-performance native duplex
 * stream.
 *
 * ## Quick Start
 *
 * ```ts
 * import { createPort, SignalChanged } from "node-null";
 *
 * const port = await createPort({ portNumber: 10 });
 *
 * // Respond to data from the external application
 * port.stream.on("data", (chunk) => {
 *   console.log("Received:", chunk);
 *   port.stream.write(Buffer.from("ACK\r\n"));
 * });
 *
 * // Monitor serial configuration changes
 * port.on("signal", (state) => {
 *   if (state.changedMask & SignalChanged.BAUD) {
 *     console.log(`Baud rate: ${state.baudRate}`);
 *   }
 * });
 *
 * // Clean up
 * await port.destroy();
 * ```
 *
 * @module
 */

export { createPort, VirtualPort } from "./port.js";
export {
  nativeListPorts as listPorts,
  nativeIsDriverAvailable as isDriverInstalled,
  nativeGetDriverVersion as driverVersion,
} from "./control.js";
export { VirtualPortStream } from "./stream.js";
export {
  SignalChanged,
  type CompanionSignals,
  type CreatePortOptions,
  type HandFlow,
  type Parity,
  type PortInfo,
  type SignalChangedBit,
  type SignalState,
  type SpecialChars,
  type StopBits,
  type VirtualPortEventMap,
} from "./types.js";
