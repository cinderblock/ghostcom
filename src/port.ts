import { EventEmitter } from "node:events";
import { createRequire } from "node:module";

import { nativeCreatePort, nativeDestroyPort } from "./control.js";
import { decodeSignalState, type RawSignalState } from "./signals.js";
import { VirtualPortStream, type NativeStreamBinding } from "./stream.js";
import {
  SignalChanged,
  type CompanionSignals,
  type CreatePortOptions,
  type SignalState,
  type VirtualPortEventMap,
} from "./types.js";

/**
 * Native port binding — returned by the addon when opening a
 * companion device. Contains the stream and signal interfaces.
 */
export interface NativePortBinding {
  /** Create a stream binding for this port's companion device. */
  createStream(): NativeStreamBinding;

  /**
   * Register a callback for signal change notifications.
   * Called from a dedicated thread via ThreadsafeFunction.
   */
  onSignalChange(callback: (raw: RawSignalState) => void): void;

  /** Get the current signal state snapshot. */
  getSignals(): RawSignalState;

  /** Set companion-side output signals (DTR, RTS). */
  setSignals(dtr: boolean, rts: boolean): void;

  /** Shut down the signal notification thread. */
  shutdownSignals(): void;

  /** Close the companion device handle. */
  close(): void;
}

/**
 * A virtual COM port backed by the node-null kernel driver.
 *
 * This class represents a single virtual serial port that appears as
 * a real COM port to the rest of the system. External applications
 * (terminal emulators, firmware tools, etc.) can open and use it as
 * if it were a physical serial device.
 *
 * Data flows through the {@link stream} property — a high-performance
 * native {@link Duplex} stream. The byte stream is completely agnostic
 * to serial line configuration (baud rate, parity, etc.); all bytes
 * flow at native speed regardless of what the external application
 * configured.
 *
 * Serial control signals are observable via the `"signal"` event.
 * You can also set output signals with {@link setSignals}.
 *
 * @example
 * ```ts
 * import { createPort, SignalChanged } from "node-null";
 *
 * const port = await createPort({ portNumber: 10 });
 * console.log(`Created ${port.portName}`);
 *
 * port.on("com-open", () => {
 *   console.log("External app connected");
 *   port.setSignals({ dtr: true, rts: true });
 * });
 *
 * port.on("signal", (state) => {
 *   if (state.changedMask & SignalChanged.BAUD) {
 *     console.log(`Baud rate: ${state.baudRate}`);
 *   }
 * });
 *
 * // Echo all received data back
 * port.stream.pipe(port.stream);
 *
 * // Cleanup
 * process.on("SIGINT", async () => {
 *   await port.destroy();
 * });
 * ```
 */
export class VirtualPort extends EventEmitter<VirtualPortEventMap> {
  /** The COM port name (e.g., "COM10"). */
  readonly portName: string;

  /** The COM port number (e.g., 10). */
  readonly portNumber: number;

  /** The duplex byte stream connected to this virtual port. */
  readonly stream: VirtualPortStream;

  readonly #companionIndex: number;
  readonly #native: NativePortBinding;

  #signalState: SignalState | undefined;
  #destroyed = false;

  /** @internal — use {@link createPort} instead. */
  constructor(
    portNumber: number,
    companionIndex: number,
    native: NativePortBinding,
  ) {
    super();

    this.portNumber = portNumber;
    this.portName = `COM${portNumber}`;
    this.#companionIndex = companionIndex;
    this.#native = native;

    // Create the native duplex stream binding
    const streamBinding = native.createStream();
    this.stream = new VirtualPortStream(streamBinding);

    // Forward stream errors
    this.stream.on("error", (err) => {
      this.emit("error", err);
    });

    // Start the signal notification thread
    native.onSignalChange((raw: RawSignalState) => {
      const state = decodeSignalState(raw);
      this.#signalState = state;

      // Emit the general signal event
      this.emit("signal", state);

      // Emit convenience events for open/close
      if (state.changedMask & SignalChanged.COM_OPEN) {
        this.emit("com-open");
      }
      if (state.changedMask & SignalChanged.COM_CLOSE) {
        this.emit("com-close");
      }
    });

    // Eagerly load the initial signal state
    try {
      this.#signalState = decodeSignalState(native.getSignals());
    } catch {
      // Driver may not have a COM-side connection yet; that's fine.
    }
  }

  /**
   * The current signal state snapshot, or `undefined` if no state
   * has been received yet.
   */
  get signals(): Readonly<SignalState> | undefined {
    return this.#signalState;
  }

  /**
   * Whether an external application currently has the COM port open.
   * This is derived from the last signal state update.
   */
  get isComOpen(): boolean {
    return this.#signalState?.dtr === true;
  }

  /**
   * Whether this port has been destroyed.
   */
  get destroyed(): boolean {
    return this.#destroyed;
  }

  /**
   * Set modem output signals visible to the COM-side application.
   *
   * Through null-modem crossover:
   * - `dtr: true` → COM side sees DSR + DCD asserted
   * - `rts: true` → COM side sees CTS asserted
   *
   * @param signals - Which signals to assert or clear.
   */
  setSignals(signals: CompanionSignals): void {
    if (this.#destroyed) {
      throw new Error("Port has been destroyed");
    }

    this.#native.setSignals(
      signals.dtr ?? this.#signalState?.dtr ?? false,
      signals.rts ?? this.#signalState?.rts ?? false,
    );
  }

  /**
   * Destroy this virtual port.
   *
   * This closes the companion device, tears down the COM port in the
   * driver, and removes it from the system. Any external application
   * that had the port open will see it disappear.
   *
   * After calling this method, the port is no longer usable.
   */
  async destroy(): Promise<void> {
    if (this.#destroyed) return;
    this.#destroyed = true;

    // Shut down the stream (cancels pending I/O)
    this.stream.destroy();

    // Stop signal notifications
    this.#native.shutdownSignals();

    // Close the companion device
    this.#native.close();

    // Tell the driver to remove the virtual port
    try {
      nativeDestroyPort(this.#companionIndex);
    } catch {
      // If the driver already removed it, that's fine.
    }

    this.removeAllListeners();
  }
}

/**
 * Create a new virtual COM port.
 *
 * The port appears immediately in Device Manager and is available for
 * any application to open. Returns a {@link VirtualPort} with a
 * high-performance duplex stream and control signal event emitter.
 *
 * @param options - Optional configuration for the new port.
 * @returns The newly created virtual port.
 *
 * @throws If the vcom driver is not installed.
 * @throws If the requested port number is already in use.
 *
 * @example
 * ```ts
 * const port = await createPort({ portNumber: 10 });
 * console.log(port.portName); // "COM10"
 *
 * port.stream.on("data", (chunk) => {
 *   console.log("Received:", chunk);
 * });
 *
 * port.stream.write(Buffer.from("Hello from node-null!\r\n"));
 * ```
 */
export async function createPort(
  options?: CreatePortOptions,
): Promise<VirtualPort> {
  const requestedPort = options?.portNumber ?? 0;

  // Create the port in the driver
  const result = nativeCreatePort(requestedPort);

  // Open the companion device and create native bindings.
  // Use createRequire since this is an ESM module loading a .node addon.
  const require = createRequire(import.meta.url);
  const native = require("../addon/node-null.node");
  const portBinding: NativePortBinding = native.openPort(
    result.companionIndex,
  );

  return new VirtualPort(result.portNumber, result.companionIndex, portBinding);
}
