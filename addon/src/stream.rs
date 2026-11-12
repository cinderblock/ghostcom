//! Native stream implementation — bridges overlapped I/O on the
//! companion device handle to Node.js readable/writable stream
//! callbacks via ThreadsafeFunction.
//!
//! ## Architecture
//!
//! ```text
//!  JS thread                  Read thread
//!  ────────                  ────────────
//!  stream._read() ──────►   resume event
//!                           ReadFile (overlapped, blocks)
//!  push(chunk) ◄──────────  ThreadsafeFunction callback
//!  (backpressure) ────────► pause (atomic flag)
//!
//!  stream._write(chunk) ──► WriteFile (overlapped, on worker thread)
//!  callback(err) ◄────────  completion
//! ```

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread::{self, JoinHandle};

use napi::bindgen_prelude::*;
use napi::threadsafe_function::{ThreadsafeFunction, ThreadsafeFunctionCallMode};
use napi_derive::napi;
use std::sync::Mutex;
use windows::Win32::Foundation::{ERROR_IO_PENDING, ERROR_OPERATION_ABORTED};
use windows::Win32::Storage::FileSystem::{ReadFile, WriteFile};

use crate::error::SendHandle;
use crate::overlapped::{cancel_io, OverlappedEvent};

/// Read buffer size — matches the driver's ring buffer default.
const READ_BUF_SIZE: usize = 64 * 1024;

/// Threadsafe function delivering read chunks to JavaScript.
///
/// `CalleeHandled = false` (napi v2's `ErrorStrategy::Fatal`): the JS callback
/// receives `(buffer)` directly, with no leading error argument. A
/// callee-handled function would call `(null, buffer)`, and chunk=null would
/// call push(null), which signals EOF and permanently closes the readable
/// stream.
type DataTsfn = ThreadsafeFunction<Buffer, (), Buffer, Status, false>;

/// Threadsafe function delivering read errors to JavaScript as
/// `(errorString)`. Not callee-handled, for the same reason as [`DataTsfn`].
type ErrorTsfn = ThreadsafeFunction<String, (), String, Status, false>;

/// Threadsafe function for write completion. Callee-handled (napi v2's
/// `ErrorStrategy::CalleeHandled`): the JS callback receives `(null)` on
/// success and `(Error)` on failure, the Node.js stream `_write` convention.
type WriteTsfn = ThreadsafeFunction<(), (), (), Status, true>;

/// Native stream binding exposed to JavaScript.
///
/// The TypeScript `VirtualPortStream` wraps this to implement `Duplex`.
#[napi]
pub struct PortStreamNative {
    handle: SendHandle,
    shutdown: Arc<AtomicBool>,
    paused: Arc<AtomicBool>,
    read_thread: Option<JoinHandle<()>>,
    // Store tsfn references so we can abort them on shutdown.
    // The data tsfn is shared with the read thread, which holds the other
    // reference until it exits.
    data_tsfn: Option<Arc<DataTsfn>>,
    // Shared with the read thread so it can report errors.
    error_tsfn: Arc<Mutex<Option<ErrorTsfn>>>,
}

impl PortStreamNative {
    /// Create a new native stream for the given companion device handle.
    pub fn new(handle: SendHandle) -> Self {
        Self {
            handle,
            shutdown: Arc::new(AtomicBool::new(false)),
            paused: Arc::new(AtomicBool::new(false)),
            read_thread: None,
            data_tsfn: None,
            error_tsfn: Arc::new(Mutex::new(None)),
        }
    }
}

#[napi]
impl PortStreamNative {
    /// Register the data callback. This starts the read thread.
    ///
    /// Called once from the TypeScript constructor.
    #[napi]
    pub fn on_data(&mut self, callback: Function<'_, Buffer, ()>) -> Result<()> {
        // Not callee-handled: JS callback receives (buffer) directly. See DataTsfn.
        let tsfn: Arc<DataTsfn> = Arc::new(
            callback
                .build_threadsafe_function()
                .callee_handled::<false>()
                .build_callback(|ctx| Ok(ctx.value))?,
        );

        let handle = self.handle;
        let shutdown = self.shutdown.clone();
        let paused = self.paused.clone();
        let tsfn_clone = tsfn.clone();
        let error_tsfn = self.error_tsfn.clone();

        let read_thread = thread::Builder::new()
            .name("gcom-reader".into())
            .spawn(move || {
                Self::read_thread_main(handle, tsfn_clone, error_tsfn, shutdown, paused);
            })
            .map_err(|e| {
                napi::Error::new(
                    napi::Status::GenericFailure,
                    format!("Failed to spawn read thread: {e}"),
                )
            })?;

        self.read_thread = Some(read_thread);
        self.data_tsfn = Some(tsfn);
        Ok(())
    }

    /// Register the read error callback.
    ///
    /// Must be called before any data arrives (immediately after on_data).
    #[napi]
    pub fn on_read_error(&mut self, callback: Function<'_, String, ()>) -> Result<()> {
        // Not callee-handled: JS callback receives (errorString) directly.
        let tsfn: ErrorTsfn = callback
            .build_threadsafe_function()
            .callee_handled::<false>()
            .build_callback(|ctx| Ok(ctx.value))?;
        *self.error_tsfn.lock().unwrap() = Some(tsfn);
        Ok(())
    }

    /// Write a buffer to the companion device (overlapped I/O).
    ///
    /// The callback is invoked when the write completes, following Node.js
    /// convention: callback(null) on success, callback(Error) on failure.
    #[napi]
    pub fn write(&self, chunk: Buffer, callback: Function<'_, Option<Error>, ()>) -> Result<()> {
        let handle = self.handle;
        let data: Vec<u8> = chunk.to_vec();

        // Callee-handled so the JS callback receives (null) on success and
        // (Error) on failure, exactly the Node.js stream _write convention.
        // ctx.value is (), which napi 3 passes as no arguments, so success is
        // exactly (null). (napi 2 passed (null, undefined); the write callback
        // ignores the second arg, so the two are the same in practice.)
        let tsfn: WriteTsfn = callback
            .build_threadsafe_function()
            .callee_handled::<true>()
            .build_callback(|_ctx| Ok(()))?;

        // Perform the write on a worker thread to avoid blocking the
        // JS event loop.
        thread::Builder::new()
            .name("gcom-writer".into())
            .spawn(move || {
                let result = Self::do_write(handle, &data);
                match result {
                    Ok(()) => {
                        // callback(null) — success
                        tsfn.call(Ok(()), ThreadsafeFunctionCallMode::NonBlocking);
                    }
                    Err(e) => {
                        // callback(Error) — failure
                        tsfn.call(
                            Err(napi::Error::new(
                                napi::Status::GenericFailure,
                                e.to_string(),
                            )),
                            ThreadsafeFunctionCallMode::NonBlocking,
                        );
                    }
                }
            })
            .map_err(|e| {
                napi::Error::new(
                    napi::Status::GenericFailure,
                    format!("Failed to spawn write thread: {e}"),
                )
            })?;

        Ok(())
    }

    /// Pause reading (backpressure from the JS side).
    #[napi]
    pub fn pause_reading(&self) {
        self.paused.store(true, Ordering::Release);
    }

    /// Resume reading after a pause.
    #[napi]
    pub fn resume_reading(&self) {
        self.paused.store(false, Ordering::Release);
    }

    /// Shut down the stream: cancel pending I/O, join threads.
    #[napi]
    pub fn shutdown(&mut self) {
        self.shutdown.store(true, Ordering::Release);

        // Cancel pending I/O so the read thread unblocks.
        let _ = cancel_io(self.handle.raw());

        // Join the read thread.
        if let Some(thread) = self.read_thread.take() {
            let _ = thread.join();
        }

        // Abort the tsfn references so the process can exit.
        //
        // `abort` is deprecated in napi 3 in favour of releasing on drop, but
        // abort also closes the queue so no further callbacks reach JS after
        // shutdown, which is the behavior we want to keep.
        if let Some(tsfn) = self.data_tsfn.take() {
            // The read thread has been joined, so its reference is gone and
            // this is the last one. If that ever isn't true, dropping our
            // reference still releases it.
            if let Ok(tsfn) = Arc::try_unwrap(tsfn) {
                #[allow(deprecated)]
                tsfn.abort().ok();
            }
        }
        if let Some(tsfn) = self.error_tsfn.lock().unwrap().take() {
            #[allow(deprecated)]
            tsfn.abort().ok();
        }
    }

    /// The read thread's main loop.
    fn read_thread_main(
        handle: SendHandle,
        tsfn: Arc<DataTsfn>,
        error_tsfn: Arc<Mutex<Option<ErrorTsfn>>>,
        shutdown: Arc<AtomicBool>,
        paused: Arc<AtomicBool>,
    ) {
        let mut overlapped = match OverlappedEvent::new() {
            Ok(o) => o,
            Err(e) => {
                eprintln!("ghostcom: read thread failed to create overlapped event: {e}");
                return;
            }
        };

        let mut buffer = vec![0u8; READ_BUF_SIZE];

        loop {
            if shutdown.load(Ordering::Acquire) {
                break;
            }

            // If paused (backpressure), spin-wait with a brief sleep.
            while paused.load(Ordering::Acquire) {
                if shutdown.load(Ordering::Acquire) {
                    return;
                }
                thread::sleep(std::time::Duration::from_millis(1));
            }

            // Reset the overlapped event.
            if let Err(e) = overlapped.reset() {
                eprintln!("ghostcom: read thread reset error: {e}");
                break;
            }

            // Issue overlapped ReadFile.
            let mut bytes_read: u32 = 0;
            let read_ok = unsafe {
                ReadFile(
                    handle.raw(),
                    Some(&mut buffer),
                    Some(&mut bytes_read),
                    Some(overlapped.as_mut_ptr()),
                )
            };

            match read_ok {
                Ok(()) => {
                    // Completed synchronously — data is ready.
                }
                Err(e) if e.code() == ERROR_IO_PENDING.into() => {
                    // Pending — wait for completion.
                    match overlapped.wait_infinite() {
                        Ok(()) => match overlapped.get_result(handle.raw()) {
                            Ok(n) => bytes_read = n,
                            Err(_) => {
                                if shutdown.load(Ordering::Acquire) {
                                    break;
                                }
                                continue;
                            }
                        },
                        Err(_) => {
                            if shutdown.load(Ordering::Acquire) {
                                break;
                            }
                            continue;
                        }
                    }
                }
                Err(e) => {
                    if shutdown.load(Ordering::Acquire) {
                        break;
                    }
                    if e.code() == ERROR_OPERATION_ABORTED.into() {
                        break;
                    }
                    let msg = format!("ghostcom read error: {e}");
                    eprintln!("{msg}");
                    if let Ok(guard) = error_tsfn.lock() {
                        if let Some(ref etsfn) = *guard {
                            etsfn.call(msg, ThreadsafeFunctionCallMode::NonBlocking);
                        }
                    }
                    break;
                }
            }

            if bytes_read > 0 {
                let chunk = Buffer::from(&buffer[..bytes_read as usize]);
                // Not callee-handled: call takes the value directly (no Ok() wrapper).
                tsfn.call(chunk, ThreadsafeFunctionCallMode::NonBlocking);
            }
        }
    }

    /// Perform a synchronous-overlapped write of ALL bytes in `data`.
    ///
    /// The driver's ring buffer has `GCOM_RING_BUFFER_SIZE - 1` capacity.
    /// A single WriteFile may accept fewer bytes than requested when the ring
    /// is nearly full. This function retries until every byte is written or an
    /// error occurs, so callers always get a fully-written guarantee.
    fn do_write(handle: SendHandle, data: &[u8]) -> napi::Result<()> {
        let mut offset = 0usize;

        while offset < data.len() {
            let mut overlapped = OverlappedEvent::new()?;
            overlapped.reset()?;

            let remaining = &data[offset..];
            let mut bytes_written: u32 = 0;

            let write_ok = unsafe {
                WriteFile(
                    handle.raw(),
                    Some(remaining),
                    Some(&mut bytes_written),
                    Some(overlapped.as_mut_ptr()),
                )
            };

            let written = match write_ok {
                Ok(()) => bytes_written as usize,
                Err(e) if e.code() == ERROR_IO_PENDING.into() => {
                    overlapped.wait_infinite()?;
                    overlapped.get_result(handle.raw())? as usize
                }
                Err(e) => return Err(crate::error::win_err("WriteFile", e)),
            };

            if written == 0 {
                // Driver accepted 0 bytes — ring full and no progress.
                // Yield briefly and retry rather than spinning hard.
                std::thread::sleep(std::time::Duration::from_millis(1));
            } else {
                offset += written;
            }
        }

        Ok(())
    }
}

impl Drop for PortStreamNative {
    fn drop(&mut self) {
        self.shutdown();
    }
}
