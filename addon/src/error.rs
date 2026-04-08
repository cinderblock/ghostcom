//! Error types and conversions for the native addon.

use napi::Status;
use windows::core::Error as WinError;
use windows::Win32::Foundation::HANDLE;

/// Convert a Windows API error into a napi::Error with a descriptive message.
pub fn win_err(context: &str, err: WinError) -> napi::Error {
    napi::Error::new(
        Status::GenericFailure,
        format!("{context}: {err} (HRESULT 0x{:08X})", err.code().0),
    )
}

/// A wrapper around `HANDLE` that implements `Send`.
///
/// Windows kernel object handles are valid process-wide and safe to use
/// from any thread. The `windows` crate doesn't impl Send for HANDLE
/// because it's a raw pointer, but for device handles this is safe.
#[derive(Debug, Clone, Copy)]
pub struct SendHandle(pub HANDLE);

unsafe impl Send for SendHandle {}
unsafe impl Sync for SendHandle {}

impl SendHandle {
    pub fn raw(&self) -> HANDLE {
        self.0
    }

    pub fn is_invalid(&self) -> bool {
        self.0.is_invalid()
    }
}

/// Create a napi::Error for when the driver is not available.
pub fn driver_not_found() -> napi::Error {
    napi::Error::new(
        Status::GenericFailure,
        "The node-null virtual COM port driver is not installed. \
         Install it with `bun run install:driver` (requires administrator).",
    )
}

/// Create a napi::Error for an invalid companion index.
pub fn invalid_companion(index: u32) -> napi::Error {
    napi::Error::new(
        Status::InvalidArg,
        format!("Invalid companion device index: {index}"),
    )
}
