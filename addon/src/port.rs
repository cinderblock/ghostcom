//! Per-port native binding — manages the companion device handle,
//! stream, and signal watcher for a single virtual port.

use std::mem;

use napi::bindgen_prelude::*;
use napi_derive::napi;
use windows::Win32::Foundation::{CloseHandle, HANDLE};

use crate::error::{self, SendHandle};
use crate::ioctl::*;
use crate::overlapped::{device_ioctl, open_device_overlapped, open_device_sync};
use crate::signals::{RawSignalStateJs, SignalWatcher};
use crate::stream::PortStreamNative;

/// Native port handle — wraps the companion device and provides
/// stream/signal interfaces to JavaScript.
#[napi]
pub struct NativePort {
    /// Handle for overlapped data I/O (ReadFile/WriteFile).
    data_handle: SendHandle,

    /// Handle for synchronous IOCTLs (GET_SIGNALS, SET_SIGNALS).
    /// We use a separate handle because mixing overlapped and
    /// synchronous I/O on the same handle is problematic.
    control_handle: HANDLE,

    #[allow(dead_code)]
    companion_index: u32,
    signal_watcher: Option<SignalWatcher>,
}

/// Open a companion device and return a NativePort.
///
/// Called from JavaScript after `createPort()` allocates the port
/// pair in the driver.
#[napi]
pub fn open_port(companion_index: u32) -> Result<NativePort> {
    let path = format!("\\\\.\\GCOM{}", companion_index);

    // Open two handles: one for overlapped data I/O, one for sync control.
    let data_handle = open_device_overlapped(&path)
        .map_err(|_| error::invalid_companion(companion_index))?;

    let control_handle = open_device_sync(&path)
        .map_err(|e| {
            unsafe { let _ = CloseHandle(data_handle); }
            e
        })?;

    Ok(NativePort {
        data_handle: SendHandle(data_handle),
        control_handle,
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
        Ok(PortStreamNative::new(self.data_handle))
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

        let tsfn = callback.create_threadsafe_function(
            0,
            |ctx: napi::threadsafe_function::ThreadSafeCallContext<RawSignalStateJs>| {
                Ok(vec![ctx.value])
            },
        )?;

        self.signal_watcher = Some(SignalWatcher::start(self.data_handle, tsfn));
        Ok(())
    }

    /// Get the current signal state (synchronous).
    #[napi]
    pub fn get_signals(&self) -> Result<RawSignalStateJs> {
        let mut state = GcomSignalState::default();

        unsafe {
            device_ioctl(
                self.control_handle,
                IOCTL_GCOM_GET_SIGNALS,
                std::ptr::null(),
                0,
                &mut state as *mut _ as *mut u8,
                mem::size_of::<GcomSignalState>() as u32,
            )?;
        }

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

        unsafe {
            device_ioctl(
                self.control_handle,
                IOCTL_GCOM_SET_SIGNALS,
                &signals as *const _ as *const u8,
                mem::size_of::<GcomSetSignals>() as u32,
                std::ptr::null_mut(),
                0,
            )?;
        }

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
        // Stop signal watcher first (it uses the data handle).
        self.shutdown_signals();

        // Close the data handle.
        if !self.data_handle.is_invalid() {
            unsafe { let _ = CloseHandle(self.data_handle.raw()); }
            self.data_handle = SendHandle(HANDLE::default());
        }

        // Close the control handle.
        if !self.control_handle.is_invalid() {
            unsafe { let _ = CloseHandle(self.control_handle); }
            self.control_handle = HANDLE::default();
        }
    }
}

impl Drop for NativePort {
    fn drop(&mut self) {
        self.close();
    }
}
