//! Signal change notification thread.
//!
//! Runs a dedicated OS thread that issues overlapped
//! IOCTL_VCOM_WAIT_SIGNAL_CHANGE requests to the companion device.
//! When the driver completes the request (because a signal changed
//! on the COM side), the thread invokes a ThreadsafeFunction to
//! deliver the signal state snapshot to the JavaScript event loop.

use std::mem;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

use napi_derive::napi;
use napi::threadsafe_function::{
    ErrorStrategy, ThreadsafeFunction, ThreadsafeFunctionCallMode,
};

use crate::error::SendHandle;
use crate::ioctl::*;
use crate::overlapped::{device_ioctl_overlapped, OverlappedEvent};

/// Raw signal state object passed to JavaScript.
///
/// Field names match what the TypeScript `RawSignalState` interface expects.
#[napi(object)]
pub struct RawSignalStateJs {
    pub sequence_number: u32,
    pub changed_mask: u32,
    pub baud_rate: u32,
    pub data_bits: u32,
    pub stop_bits: u32,
    pub parity: u32,
    pub dtr_state: bool,
    pub rts_state: bool,
    pub break_state: bool,
    pub control_handshake: u32,
    pub flow_replace: u32,
    pub xon_limit: i32,
    pub xoff_limit: i32,
    pub eof_char: u32,
    pub error_char: u32,
    pub break_char: u32,
    pub event_char: u32,
    pub xon_char: u32,
    pub xoff_char: u32,
    pub wait_mask: u32,
}

impl From<&VcomSignalState> for RawSignalStateJs {
    fn from(s: &VcomSignalState) -> Self {
        Self {
            sequence_number: s.sequence_number,
            changed_mask: s.changed_mask,
            baud_rate: s.baud_rate,
            data_bits: s.data_bits as u32,
            stop_bits: s.stop_bits as u32,
            parity: s.parity as u32,
            dtr_state: s.dtr_state != 0,
            rts_state: s.rts_state != 0,
            break_state: s.break_state != 0,
            control_handshake: s.control_handshake,
            flow_replace: s.flow_replace,
            xon_limit: s.xon_limit,
            xoff_limit: s.xoff_limit,
            eof_char: s.eof_char as u32,
            error_char: s.error_char as u32,
            break_char: s.break_char as u32,
            event_char: s.event_char as u32,
            xon_char: s.xon_char as u32,
            xoff_char: s.xoff_char as u32,
            wait_mask: s.wait_mask,
        }
    }
}

/// Managed signal notification thread.
pub struct SignalWatcher {
    shutdown: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
}

impl SignalWatcher {
    /// Start the signal notification thread.
    ///
    /// `handle` must be a valid, overlapped-capable handle to a
    /// companion device (`\\.\VCOMCompanion<N>`).
    ///
    /// `callback` is a ThreadsafeFunction that will be called on the
    /// JS event loop thread whenever a signal changes.
    pub fn start(
        handle: SendHandle,
        callback: ThreadsafeFunction<RawSignalStateJs, ErrorStrategy::CalleeHandled>,
    ) -> Self {
        let shutdown = Arc::new(AtomicBool::new(false));
        let shutdown_clone = shutdown.clone();

        let thread = thread::Builder::new()
            .name("vcom-signal-watcher".into())
            .spawn(move || {
                Self::thread_main(handle, callback, shutdown_clone);
            })
            .expect("failed to spawn signal watcher thread");

        Self {
            shutdown,
            thread: Some(thread),
        }
    }

    /// Signal the thread to shut down and wait for it to exit.
    pub fn stop(&mut self) {
        self.shutdown.store(true, Ordering::Release);

        if let Some(thread) = self.thread.take() {
            let _ = thread.join();
        }
    }

    /// The thread's main loop.
    fn thread_main(
        handle: SendHandle,
        callback: ThreadsafeFunction<RawSignalStateJs, ErrorStrategy::CalleeHandled>,
        shutdown: Arc<AtomicBool>,
    ) {
        let mut overlapped = match OverlappedEvent::new() {
            Ok(o) => o,
            Err(e) => {
                eprintln!("node-null: failed to create overlapped event for signal watcher: {e}");
                return;
            }
        };

        loop {
            if shutdown.load(Ordering::Acquire) {
                break;
            }

            let mut signal_state = VcomSignalState::default();

            // Issue the overlapped WAIT_SIGNAL_CHANGE IOCTL.
            let ioctl_result = unsafe {
                device_ioctl_overlapped(
                    handle.raw(),
                    IOCTL_VCOM_WAIT_SIGNAL_CHANGE,
                    std::ptr::null(),
                    0,
                    &mut signal_state as *mut _ as *mut u8,
                    mem::size_of::<VcomSignalState>() as u32,
                    &mut overlapped,
                )
            };

            if let Err(e) = ioctl_result {
                if shutdown.load(Ordering::Acquire) {
                    break;
                }
                eprintln!("node-null: signal watcher IOCTL failed: {e}");
                thread::sleep(std::time::Duration::from_millis(100));
                continue;
            }

            // Wait for the IOCTL to complete (blocks this thread).
            match overlapped.wait_infinite() {
                Ok(()) => {}
                Err(_) => {
                    if shutdown.load(Ordering::Acquire) {
                        break;
                    }
                    continue;
                }
            }

            // Get the result (byte count).
            match overlapped.get_result(handle.raw()) {
                Ok(bytes) => {
                    if bytes as usize >= mem::size_of::<VcomSignalState>() {
                        let js_state = RawSignalStateJs::from(&signal_state);
                        callback.call(Ok(js_state), ThreadsafeFunctionCallMode::NonBlocking);
                    }
                }
                Err(_) => {
                    if shutdown.load(Ordering::Acquire) {
                        break;
                    }
                    continue;
                }
            }
        }
    }
}

impl Drop for SignalWatcher {
    fn drop(&mut self) {
        self.stop();
    }
}
