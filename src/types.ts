/**
 * Options for creating a new virtual COM port.
 */
export interface CreatePortOptions {
  /**
   * Specific COM port number to assign (e.g., 10 for COM10).
   * If omitted, the driver will auto-assign the lowest available number.
   */
  portNumber?: number;
}

/**
 * Information about an active virtual COM port.
 */
export interface PortInfo {
  /** The assigned COM port number (e.g., 10 for COM10). */
  portNumber: number;

  /** Internal companion device index. */
  companionIndex: number;

  /** Whether an external application currently has the COM port open. */
  comSideOpen: boolean;

  /** Whether the companion (node-null) side is currently connected. */
  companionSideOpen: boolean;
}

/**
 * Parity mode for the serial line.
 */
export type Parity = "none" | "odd" | "even" | "mark" | "space";

/**
 * Stop bits configuration.
 */
export type StopBits = "one" | "one-five" | "two";

/**
 * Flow control handshake configuration as reported by the COM side.
 */
export interface HandFlow {
  /** Raw SERIAL_HANDFLOW.ControlHandShake bitmask. */
  controlHandshake: number;

  /** Raw SERIAL_HANDFLOW.FlowReplace bitmask. */
  flowReplace: number;

  /** XON buffer threshold. */
  xonLimit: number;

  /** XOFF buffer threshold. */
  xoffLimit: number;
}

/**
 * Special characters configured on the serial port.
 */
export interface SpecialChars {
  eofChar: number;
  errorChar: number;
  breakChar: number;
  eventChar: number;
  xonChar: number;
  xoffChar: number;
}

/**
 * Complete snapshot of all serial control signal state.
 *
 * This is returned on every signal change event and represents the
 * full state of what the COM-side application has configured.
 *
 * The byte stream is agnostic to these settings — they are purely
 * informational and "tappable" by your application.
 */
export interface SignalState {
  /** Monotonically increasing counter. Increments on any change. */
  sequenceNumber: number;

  /** Bitmask of what changed since the last notification. */
  changedMask: number;

  /** Baud rate in bits per second (e.g., 9600, 115200). */
  baudRate: number;

  /** Data bits per character (5, 6, 7, or 8). */
  dataBits: 5 | 6 | 7 | 8;

  /** Stop bits configuration. */
  stopBits: StopBits;

  /** Parity mode. */
  parity: Parity;

  /** Data Terminal Ready — asserted by the COM-side application. */
  dtr: boolean;

  /** Request To Send — asserted by the COM-side application. */
  rts: boolean;

  /** Whether the COM side has asserted a line break. */
  breakState: boolean;

  /** Flow control configuration. */
  handflow: HandFlow;

  /** Special characters (XON, XOFF, etc.). */
  specialChars: SpecialChars;

  /** The COM-side's event wait mask (EV_RXCHAR, EV_TXEMPTY, etc.). */
  waitMask: number;
}

/**
 * Bitmask constants for `SignalState.changedMask`.
 *
 * Use these to determine exactly what changed in a signal event:
 *
 * ```ts
 * port.on("signal", (state) => {
 *   if (state.changedMask & SignalChanged.BAUD) {
 *     console.log(`Baud rate is now ${state.baudRate}`);
 *   }
 * });
 * ```
 */
export const SignalChanged = {
  /** Baud rate was changed. */
  BAUD: 0x0001,

  /** Data bits, stop bits, or parity was changed. */
  LINE_CONTROL: 0x0002,

  /** DTR was asserted or cleared. */
  DTR: 0x0004,

  /** RTS was asserted or cleared. */
  RTS: 0x0008,

  /** Break was set or cleared. */
  BREAK: 0x0010,

  /** Flow control (handshake) configuration changed. */
  HANDFLOW: 0x0020,

  /** Special characters (XON/XOFF, etc.) changed. */
  CHARS: 0x0040,

  /** The COM side's WaitCommEvent mask changed. */
  WAIT_MASK: 0x0080,

  /** An external application opened the COM port. */
  COM_OPEN: 0x0100,

  /** The external application closed the COM port. */
  COM_CLOSE: 0x0200,
} as const;

export type SignalChangedBit =
  (typeof SignalChanged)[keyof typeof SignalChanged];

/**
 * Modem output signals that the companion side can assert.
 *
 * Through null-modem crossover, these appear on the COM side as:
 * - `dtr` → COM side sees DSR + DCD
 * - `rts` → COM side sees CTS
 */
export interface CompanionSignals {
  dtr?: boolean;
  rts?: boolean;
}

/**
 * Event map for VirtualPort.
 */
export interface VirtualPortEventMap {
  /**
   * Emitted whenever the COM-side application changes any serial
   * configuration or control signal.
   */
  signal: [state: SignalState];

  /** Emitted when an external application opens the COM port. */
  "com-open": [];

  /** Emitted when the external application closes the COM port. */
  "com-close": [];

  /** Emitted on unrecoverable errors. */
  error: [error: Error];
}
