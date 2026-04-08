//! Windows overlapped I/O helpers.
//!
//! Provides a safe wrapper around `OVERLAPPED` structures with their
//! associated event handles, used for asynchronous device I/O on the
//! companion device.

use std::mem;

use windows::Win32::Foundation::{CloseHandle, HANDLE, WAIT_OBJECT_0};
use windows::Win32::System::IO::{GetOverlappedResult, OVERLAPPED};
use windows::Win32::System::Threading::{CreateEventW, ResetEvent, SetEvent, WaitForSingleObject};

/// An OVERLAPPED structure with an owned event handle.
///
/// Used for overlapped ReadFile / WriteFile / DeviceIoControl operations.
pub struct OverlappedEvent {
    overlapped: OVERLAPPED,
    event: HANDLE,
}

// OVERLAPPED contains raw pointers (HANDLE), but we only use it from
// a single thread at a time and always through Windows APIs.
unsafe impl Send for OverlappedEvent {}

impl OverlappedEvent {
    /// Create a new overlapped structure with a manual-reset event.
    pub fn new() -> napi::Result<Self> {
        let event = unsafe {
            CreateEventW(None, true, false, None)
                .map_err(|e| crate::error::win_err("CreateEvent", e))?
        };

        let mut overlapped: OVERLAPPED = unsafe { mem::zeroed() };
        overlapped.hEvent = event;

        Ok(Self { overlapped, event })
    }

    /// Get a mutable pointer to the OVERLAPPED structure for use in
    /// Windows API calls.
    pub fn as_mut_ptr(&mut self) -> *mut OVERLAPPED {
        &mut self.overlapped as *mut OVERLAPPED
    }

    /// Reset the event to non-signaled state before starting a new
    /// overlapped operation.
    pub fn reset(&self) -> napi::Result<()> {
        unsafe {
            ResetEvent(self.event)
                .map_err(|e| crate::error::win_err("ResetEvent", e))?;
        }
        Ok(())
    }

    /// Wait for the overlapped operation to complete.
    ///
    /// Blocks the calling thread until the event is signaled or the
    /// timeout expires. Returns `true` if the operation completed,
    /// `false` on timeout.
    #[allow(dead_code)]
    pub fn wait(&self, timeout_ms: u32) -> napi::Result<bool> {
        let result = unsafe { WaitForSingleObject(self.event, timeout_ms) };
        Ok(result == WAIT_OBJECT_0)
    }

    /// Wait indefinitely for the overlapped operation to complete.
    pub fn wait_infinite(&self) -> napi::Result<()> {
        let result = unsafe { WaitForSingleObject(self.event, u32::MAX) };
        if result != WAIT_OBJECT_0 {
            return Err(napi::Error::new(
                napi::Status::GenericFailure,
                format!("WaitForSingleObject returned unexpected value: {result:?}"),
            ));
        }
        Ok(())
    }

    /// Get the result of the completed overlapped operation.
    ///
    /// Must only be called after `wait()` returned true, or after the
    /// event has been signaled.
    pub fn get_result(&mut self, handle: HANDLE) -> napi::Result<u32> {
        let mut bytes_transferred: u32 = 0;
        unsafe {
            GetOverlappedResult(handle, &self.overlapped, &mut bytes_transferred, false)
                .map_err(|e| crate::error::win_err("GetOverlappedResult", e))?;
        }
        Ok(bytes_transferred)
    }

    /// Signal this event externally (used for shutdown signaling).
    #[allow(dead_code)]
    pub fn signal(&self) -> napi::Result<()> {
        unsafe {
            SetEvent(self.event)
                .map_err(|e| crate::error::win_err("SetEvent (shutdown)", e))?;
        }
        Ok(())
    }

    /// Get the raw event handle (for use in WaitForMultipleObjects).
    #[allow(dead_code)]
    pub fn event_handle(&self) -> HANDLE {
        self.event
    }
}

impl Drop for OverlappedEvent {
    fn drop(&mut self) {
        if !self.event.is_invalid() {
            unsafe {
                let _ = CloseHandle(self.event);
            }
        }
    }
}

/// Open a device by path with overlapped I/O enabled.
pub fn open_device_overlapped(path: &str) -> napi::Result<HANDLE> {
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, FILE_FLAG_OVERLAPPED, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
    };
    use windows::Win32::Foundation::GENERIC_READ;
    // GENERIC_WRITE is 0x40000000
    const GENERIC_WRITE: u32 = 0x40000000;

    let wide_path: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    let handle = unsafe {
        CreateFileW(
            windows::core::PCWSTR(wide_path.as_ptr()),
            GENERIC_READ.0 | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            None,
        )
        .map_err(|e| crate::error::win_err(&format!("CreateFile({path})"), e))?
    };

    Ok(handle)
}

/// Open a device by path for synchronous (non-overlapped) I/O.
pub fn open_device_sync(path: &str) -> napi::Result<HANDLE> {
    use windows::Win32::Storage::FileSystem::{
        CreateFileW, FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_SHARE_WRITE, OPEN_EXISTING,
    };
    use windows::Win32::Foundation::GENERIC_READ;
    const GENERIC_WRITE: u32 = 0x40000000;

    let wide_path: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    let handle = unsafe {
        CreateFileW(
            windows::core::PCWSTR(wide_path.as_ptr()),
            GENERIC_READ.0 | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            None,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            None,
        )
        .map_err(|e| crate::error::win_err(&format!("CreateFile({path})"), e))?
    };

    Ok(handle)
}

/// Send a synchronous (non-overlapped) IOCTL to a device handle.
///
/// # Safety
/// The input/output buffers must be valid for the specified sizes and
/// must match the IOCTL's expected layout.
pub unsafe fn device_ioctl(
    handle: HANDLE,
    ioctl_code: u32,
    input: *const u8,
    input_size: u32,
    output: *mut u8,
    output_size: u32,
) -> napi::Result<u32> {
    use windows::Win32::System::IO::DeviceIoControl;

    let mut bytes_returned: u32 = 0;

    unsafe {
        DeviceIoControl(
            handle,
            ioctl_code,
            if input.is_null() {
                None
            } else {
                Some(input as *const std::ffi::c_void)
            },
            input_size,
            if output.is_null() {
                None
            } else {
                Some(output as *mut std::ffi::c_void)
            },
            output_size,
            Some(&mut bytes_returned),
            None,
        )
        .map_err(|e| crate::error::win_err("DeviceIoControl", e))?;
    }

    Ok(bytes_returned)
}

/// Send an overlapped IOCTL to a device handle.
///
/// The caller must wait on the `OverlappedEvent` for completion and
/// then call `get_result()` to retrieve the byte count.
///
/// # Safety
/// Same requirements as `device_ioctl`, plus the overlapped structure
/// must remain valid until the operation completes.
pub unsafe fn device_ioctl_overlapped(
    handle: HANDLE,
    ioctl_code: u32,
    input: *const u8,
    input_size: u32,
    output: *mut u8,
    output_size: u32,
    overlapped: &mut OverlappedEvent,
) -> napi::Result<()> {
    use windows::Win32::Foundation::ERROR_IO_PENDING;
    use windows::Win32::System::IO::DeviceIoControl;

    overlapped.reset()?;

    let result = unsafe {
        DeviceIoControl(
            handle,
            ioctl_code,
            if input.is_null() {
                None
            } else {
                Some(input as *const std::ffi::c_void)
            },
            input_size,
            if output.is_null() {
                None
            } else {
                Some(output as *mut std::ffi::c_void)
            },
            output_size,
            None,
            Some(overlapped.as_mut_ptr()),
        )
    };

    match result {
        Ok(()) => Ok(()),  // Completed synchronously
        Err(e) if e.code() == ERROR_IO_PENDING.into() => Ok(()),  // Pending — wait on event
        Err(e) => Err(crate::error::win_err("DeviceIoControl (overlapped)", e)),
    }
}

/// Cancel all pending overlapped I/O on a handle.
pub fn cancel_io(handle: HANDLE) -> napi::Result<()> {
    use windows::Win32::System::IO::CancelIoEx;

    unsafe {
        // CancelIoEx with NULL overlapped cancels all pending I/O.
        let _ = CancelIoEx(handle, None);
    }
    Ok(())
}
