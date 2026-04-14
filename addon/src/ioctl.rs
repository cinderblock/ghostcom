//! IOCTL codes and data structures shared between the kernel driver
//! and this user-mode addon.
//!
//! These definitions MUST match the driver's `ioctls.h` exactly.

#![allow(dead_code)]

use std::mem;

// ── Device type ──────────────────────────────────────────────────

/// Private device type for GCOM IOCTLs.
pub const GCOM_DEVICE_TYPE: u32 = 0x8001;

// ── CTL_CODE macro ───────────────────────────────────────────────

/// Reproduces the Windows CTL_CODE macro.
const fn ctl_code(device_type: u32, function: u32, method: u32, access: u32) -> u32 {
    (device_type << 16) | (access << 14) | (function << 2) | method
}

const METHOD_BUFFERED: u32 = 0;
const FILE_READ_ACCESS: u32 = 1;
const FILE_WRITE_ACCESS: u32 = 2;

// ── Control device IOCTLs ────────────────────────────────────────

/// Create a new virtual COM port pair (COM device + companion device).
pub const IOCTL_GCOM_CREATE_PORT: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS);

/// Destroy an existing virtual COM port pair.
pub const IOCTL_GCOM_DESTROY_PORT: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS);

/// List all active virtual COM port pairs.
pub const IOCTL_GCOM_LIST_PORTS: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS);

/// Query driver version information.
pub const IOCTL_GCOM_GET_VERSION: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS);

// ── Companion device IOCTLs ──────────────────────────────────────

/// Wait for a signal change on the COM side (overlapped / inverted call).
/// The driver holds this IRP until a signal changes, then completes it
/// with a `GcomSignalState` payload.
pub const IOCTL_GCOM_WAIT_SIGNAL_CHANGE: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS);

/// Get the current signal state snapshot (synchronous).
pub const IOCTL_GCOM_GET_SIGNALS: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x811, METHOD_BUFFERED, FILE_READ_ACCESS);

/// Set companion-side output signals (DTR, RTS) that appear on the
/// COM side through null-modem crossover.
pub const IOCTL_GCOM_SET_SIGNALS: u32 =
    ctl_code(GCOM_DEVICE_TYPE, 0x812, METHOD_BUFFERED, FILE_WRITE_ACCESS);

// ── Data structures ──────────────────────────────────────────────

/// Request payload for IOCTL_GCOM_CREATE_PORT.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomCreatePortRequest {
    /// Desired COM port number, or 0 for auto-assignment.
    pub port_number: u32,
}

/// Response payload for IOCTL_GCOM_CREATE_PORT.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomCreatePortResponse {
    /// The assigned COM port number (e.g., 10 for COM10).
    pub port_number: u32,
    /// The companion device index (used to open \\.\GCOM<N>).
    pub companion_index: u32,
}

/// Per-port status information, returned by IOCTL_GCOM_LIST_PORTS.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomPortInfo {
    pub port_number: u32,
    pub companion_index: u32,
    pub com_side_open: u32,      // BOOLEAN as u32 for alignment
    pub companion_side_open: u32, // BOOLEAN as u32 for alignment
}

/// Header for the LIST_PORTS response. Followed by `count` GcomPortInfo entries.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomListPortsHeader {
    pub count: u32,
}

/// Expected IOCTL protocol version. If the driver's protocol major
/// version differs from this, the addon should refuse to operate.
pub const GCOM_PROTOCOL_VERSION_MAJOR: u16 = 1;
pub const GCOM_PROTOCOL_VERSION_MINOR: u16 = 0;

/// Driver version response.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomVersionInfo {
    pub major: u16,
    pub minor: u16,
    pub patch: u16,
    pub protocol_major: u16,
    pub protocol_minor: u16,
    pub _reserved: u16,
}

/// Complete signal state snapshot.
///
/// Returned by IOCTL_GCOM_GET_SIGNALS and IOCTL_GCOM_WAIT_SIGNAL_CHANGE.
///
/// This structure mirrors the driver's `GCOM_SIGNAL_STATE` exactly.
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct GcomSignalState {
    /// Monotonically increasing sequence number.
    pub sequence_number: u32,

    /// Bitmask of what changed since the last WAIT completion.
    pub changed_mask: u32,

    /// Serial line configuration (set by the COM-side application).
    pub baud_rate: u32,
    pub stop_bits: u8,
    pub parity: u8,
    pub data_bits: u8,
    pub _pad0: u8,

    /// Modem control lines (set by COM-side application).
    pub dtr_state: u32, // BOOLEAN as u32
    pub rts_state: u32, // BOOLEAN as u32
    pub break_state: u32, // BOOLEAN as u32

    /// Flow control: SERIAL_HANDFLOW fields.
    pub control_handshake: u32,
    pub flow_replace: u32,
    pub xon_limit: i32,
    pub xoff_limit: i32,

    /// Special characters: SERIAL_CHARS fields.
    pub eof_char: u8,
    pub error_char: u8,
    pub break_char: u8,
    pub event_char: u8,
    pub xon_char: u8,
    pub xoff_char: u8,
    pub _pad1: [u8; 2],

    /// The COM-side's WaitCommEvent mask.
    pub wait_mask: u32,
}

impl Default for GcomSignalState {
    fn default() -> Self {
        // Safe: all-zero is a valid state (9600 baud, no signals).
        unsafe { mem::zeroed() }
    }
}

/// Payload for IOCTL_GCOM_SET_SIGNALS.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomSetSignals {
    pub dtr_state: u32, // BOOLEAN as u32
    pub rts_state: u32, // BOOLEAN as u32
}

/// Request payload for IOCTL_GCOM_DESTROY_PORT.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct GcomDestroyPortRequest {
    pub companion_index: u32,
}

// ── ChangedMask bit definitions ──────────────────────────────────

pub const GCOM_CHANGED_BAUD: u32 = 0x0001;
pub const GCOM_CHANGED_LINE_CTRL: u32 = 0x0002;
pub const GCOM_CHANGED_DTR: u32 = 0x0004;
pub const GCOM_CHANGED_RTS: u32 = 0x0008;
pub const GCOM_CHANGED_BREAK: u32 = 0x0010;
pub const GCOM_CHANGED_HANDFLOW: u32 = 0x0020;
pub const GCOM_CHANGED_CHARS: u32 = 0x0040;
pub const GCOM_CHANGED_WAIT_MASK: u32 = 0x0080;
pub const GCOM_CHANGED_COM_OPEN: u32 = 0x0100;
pub const GCOM_CHANGED_COM_CLOSE: u32 = 0x0200;
