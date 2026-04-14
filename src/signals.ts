import type { Parity, SignalState, StopBits } from "./types.js";

/**
 * Raw signal state as returned by the native addon.
 * Field names match the C `GCOM_SIGNAL_STATE` structure layout.
 */
export interface RawSignalState {
  sequenceNumber: number;
  changedMask: number;
  baudRate: number;
  dataBits: number;
  stopBits: number;
  parity: number;
  dtrState: boolean;
  rtsState: boolean;
  breakState: boolean;
  controlHandshake: number;
  flowReplace: number;
  xonLimit: number;
  xoffLimit: number;
  eofChar: number;
  errorChar: number;
  breakChar: number;
  eventChar: number;
  xonChar: number;
  xoffChar: number;
  waitMask: number;
}

// Windows serial constants (from ntddser.h)
const STOP_BIT_1 = 0;
const STOP_BIT_1_5 = 1;
const STOP_BIT_2 = 2;

const NO_PARITY = 0;
const ODD_PARITY = 1;
const EVEN_PARITY = 2;
const MARK_PARITY = 3;
const SPACE_PARITY = 4;

function decodeStopBits(raw: number): StopBits {
  switch (raw) {
    case STOP_BIT_1:
      return "one";
    case STOP_BIT_1_5:
      return "one-five";
    case STOP_BIT_2:
      return "two";
    default:
      return "one";
  }
}

function decodeParity(raw: number): Parity {
  switch (raw) {
    case NO_PARITY:
      return "none";
    case ODD_PARITY:
      return "odd";
    case EVEN_PARITY:
      return "even";
    case MARK_PARITY:
      return "mark";
    case SPACE_PARITY:
      return "space";
    default:
      return "none";
  }
}

/**
 * Convert raw native signal state into the user-friendly SignalState.
 */
export function decodeSignalState(raw: RawSignalState): SignalState {
  return {
    sequenceNumber: raw.sequenceNumber,
    changedMask: raw.changedMask,
    baudRate: raw.baudRate,
    dataBits: (raw.dataBits & 0xff) as 5 | 6 | 7 | 8,
    stopBits: decodeStopBits(raw.stopBits),
    parity: decodeParity(raw.parity),
    dtr: raw.dtrState,
    rts: raw.rtsState,
    breakState: raw.breakState,
    handflow: {
      controlHandshake: raw.controlHandshake,
      flowReplace: raw.flowReplace,
      xonLimit: raw.xonLimit,
      xoffLimit: raw.xoffLimit,
    },
    specialChars: {
      eofChar: raw.eofChar,
      errorChar: raw.errorChar,
      breakChar: raw.breakChar,
      eventChar: raw.eventChar,
      xonChar: raw.xonChar,
      xoffChar: raw.xoffChar,
    },
    waitMask: raw.waitMask,
  };
}
