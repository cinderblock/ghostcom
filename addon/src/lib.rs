//! # node-null native addon
//!
//! Rust/napi-rs bridge between the node-null kernel driver and
//! the JavaScript/TypeScript API.
//!
//! This addon provides:
//! - Control device operations (create/destroy/list ports)
//! - Per-port companion device management
//! - High-performance overlapped I/O for data streaming
//! - Signal change notification via dedicated threads

#![deny(unsafe_op_in_unsafe_fn)]

mod control;
mod error;
mod ioctl;
mod overlapped;
mod port;
mod signals;
mod stream;
