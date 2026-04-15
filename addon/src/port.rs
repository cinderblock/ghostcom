//! Per-port native binding — manages the companion device handle,
//! stream, and signal watcher for a single virtual port.
//!
//! Uses a single overlapped handle for all operations:
//! - Data I/O (ReadFile/WriteFile) — overlapped, on dedicated threads
//! - Signal IOCTLs (GET_SIGNALS, SET_SIGNALS) — overlapped + immediate wait
//! - Signal watcher (WAIT_SIGNAL_CHANGE) — overlapped, on dedicated thread

use std::mem;

use napi::bindgen_prelude::*;
use napi_derive::napi;
use windows::Win32::Foundation::{CloseHandle, HANDLE, ERROR_IO_PENDING};
use windows::Win32::System::IO::DeviceIoControl;

use crate::error::{self, SendHandle};
use crate::ioctl::*;
use crate::overlapped::{open_device_overlapped, OverlappedEvent};
use crate::signals::{RawSignalStateJs, SignalWatcher};
use crate::stream::PortStreamNative;

/// Native port handle — wraps the companion device and provides
/// stream/signal interfaces to JavaScript.
#[napi]
pub struct NativePort {
    /// Single overlapped handle for all I/O on this companion device.
    handle: SendHandle,

    #[allow(dead_code)]
    companion_index: u32,
    signal_watcher: Option<SignalWatcher>,
}

/// Perform a "synchronous" IOCTL on an overlapped handle.
///
/// Creates a temporary OVERLAPPED structure, issues the IOCTL, and
/// waits for completion. This is safe to call from any thread.
fn sync_ioctl(
    handle: HANDLE,
    ioctl_code: u32,
    input: *const u8,
    input_size: u32,
    output: *mut u8,
    output_size: u32,
) -> napi::Result<u32> {
    let mut overlapped = OverlappedEvent::new()?;
    overlapped.reset()?;

    let mut bytes_returned: u32 = 0;

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
            Some(&mut bytes_returned),
            Some(overlapped.as_mut_ptr()),
        )
    };

    match result {
        Ok(()) => Ok(bytes_returned),
        Err(e) if e.code() == ERROR_IO_PENDING.into() => {
            // Wait for the overlapped operation to complete.
            overlapped.wait_infinite()?;
            overlapped.get_result(handle)
        }
        Err(e) => Err(crate::error::win_err("DeviceIoControl", e)),
    }
}

/// Open a companion device and return a NativePort.
///
/// Called from JavaScript after `createPort()` allocates the port
/// pair in the driver.
#[napi]
pub fn open_port(companion_index: u32) -> Result<NativePort> {
    let path = format!("\\\\.\\GCOM{}", companion_index);

    // Single overlapped handle for all operations.
    let handle = open_device_overlapped(&path)
        .map_err(|_| error::invalid_companion(companion_index))?;

    Ok(NativePort {
        handle: SendHandle(handle),
        companion_index,
        signal_watcher: None,
    })
}

#[napi]
impl NativePort {
    /// Create the native stream binding.
    ///
    /// Returns a `PortStreamNative` that the TypeScript `VirtualPortStream`
    /// wraps as a `Duplex`.
    #[napi]
    pub fn create_stream(&mut self) -> Result<PortStreamNative> {
        Ok(PortStreamNative::new(self.handle))
    }

    /// Start signal change notifications.
    ///
    /// The callback is invoked on the JS event loop thread whenever
    /// the COM-side application changes any serial configuration.
    #[napi]
    pub fn on_signal_change(&mut self, callback: JsFunction) -> Result<()> {
        if self.signal_watcher.is_some() {
            return Err(napi::Error::new(
                napi::Status::GenericFailure,
                "Signal watcher already started for this port",
            ));
        }

        // ErrorStrategy::Fatal: JS callback receives (RawSignalState) directly.
        // CalleeHandled would produce (null, RawSignalState) and the signal
        // state would be silently discarded by the TypeScript callback.
        let tsfn: napi::threadsafe_function::ThreadsafeFunction<
            RawSignalStateJs,
            napi::threadsafe_function::ErrorStrategy::Fatal,
        > = callback.create_threadsafe_function(
            0,
            |ctx: napi::threadsafe_function::ThreadSafeCallContext<RawSignalStateJs>| {
                Ok(vec![ctx.value])
            },
        )?;

        self.signal_watcher = Some(SignalWatcher::start(self.handle, tsfn));
        Ok(())
    }

    /// Get the current signal state (synchronous).
    #[napi]
    pub fn get_signals(&self) -> Result<RawSignalStateJs> {
        let mut state = GcomSignalState::default();

        sync_ioctl(
            self.handle.raw(),
            IOCTL_GCOM_GET_SIGNALS,
            std::ptr::null(),
            0,
            &mut state as *mut _ as *mut u8,
            mem::size_of::<GcomSignalState>() as u32,
        )?;

        Ok(RawSignalStateJs::from(&state))
    }

    /// Set companion-side output signals.
    ///
    /// Through null-modem crossover:
    /// - DTR → COM side sees DSR + DCD
    /// - RTS → COM side sees CTS
    #[napi]
    pub fn set_signals(&self, dtr: bool, rts: bool) -> Result<()> {
        let signals = GcomSetSignals {
            dtr_state: if dtr { 1 } else { 0 },
            rts_state: if rts { 1 } else { 0 },
        };

        sync_ioctl(
            self.handle.raw(),
            IOCTL_GCOM_SET_SIGNALS,
            &signals as *const _ as *const u8,
            mem::size_of::<GcomSetSignals>() as u32,
            std::ptr::null_mut(),
            0,
        )?;

        Ok(())
    }

    /// Stop signal notifications.
    #[napi]
    pub fn shutdown_signals(&mut self) {
        if let Some(mut watcher) = self.signal_watcher.take() {
            watcher.stop();
        }
    }

    /// Close all handles and release resources.
    #[napi]
    pub fn close(&mut self) {
        // Stop signal watcher first (it uses the handle).
        self.shutdown_signals();

        if !self.handle.is_invalid() {
            unsafe { let _ = CloseHandle(self.handle.raw()); }
            self.handle = SendHandle(HANDLE::default());
        }
    }
}

impl Drop for NativePort {
    fn drop(&mut self) {
        self.close();
    }
}
