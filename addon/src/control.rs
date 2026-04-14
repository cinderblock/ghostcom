//! Control device operations — create, destroy, list virtual ports.
//!
//! These functions communicate with the driver's control device
//! (`\\.\GCOMControl`) via synchronous IOCTLs.

use std::mem;

use napi::bindgen_prelude::*;
use napi_derive::napi;
use windows::Win32::Foundation::CloseHandle;

use crate::error;
use crate::ioctl::*;
use crate::overlapped::{device_ioctl, open_device_sync};

/// Path to the driver's control device.
const CONTROL_DEVICE_PATH: &str = "\\\\.\\GCOMControl";

/// Result of creating a new virtual port.
#[napi(object)]
pub struct CreatePortResult {
    pub port_number: u32,
    pub companion_index: u32,
}

/// Information about an active virtual port.
#[napi(object)]
pub struct PortInfoJs {
    pub port_number: u32,
    pub companion_index: u32,
    pub com_side_open: bool,
    pub companion_side_open: bool,
}

/// Create a new virtual COM port pair.
///
/// Sends IOCTL_GCOM_CREATE_PORT to the control device.
///
/// `port_number`: 0 for auto-assignment, or a specific COM port number.
#[napi]
pub fn create_port(port_number: u32) -> Result<CreatePortResult> {
    let handle = open_device_sync(CONTROL_DEVICE_PATH)
        .map_err(|_| error::driver_not_found())?;

    let request = GcomCreatePortRequest { port_number };
    let mut response = GcomCreatePortResponse::default();

    let result = unsafe {
        device_ioctl(
            handle,
            IOCTL_GCOM_CREATE_PORT,
            &request as *const _ as *const u8,
            mem::size_of::<GcomCreatePortRequest>() as u32,
            &mut response as *mut _ as *mut u8,
            mem::size_of::<GcomCreatePortResponse>() as u32,
        )
    };

    unsafe { let _ = CloseHandle(handle); }

    result?;

    Ok(CreatePortResult {
        port_number: response.port_number,
        companion_index: response.companion_index,
    })
}

/// Destroy a virtual COM port pair.
///
/// Sends IOCTL_GCOM_DESTROY_PORT to the control device.
#[napi]
pub fn destroy_port(companion_index: u32) -> Result<()> {
    let handle = open_device_sync(CONTROL_DEVICE_PATH)
        .map_err(|_| error::driver_not_found())?;

    let request = GcomDestroyPortRequest { companion_index };

    let result = unsafe {
        device_ioctl(
            handle,
            IOCTL_GCOM_DESTROY_PORT,
            &request as *const _ as *const u8,
            mem::size_of::<GcomDestroyPortRequest>() as u32,
            std::ptr::null_mut(),
            0,
        )
    };

    unsafe { let _ = CloseHandle(handle); }

    result?;
    Ok(())
}

/// List all active virtual COM port pairs.
///
/// Sends IOCTL_GCOM_LIST_PORTS to the control device.
#[napi]
pub fn list_ports() -> Result<Vec<PortInfoJs>> {
    let handle = open_device_sync(CONTROL_DEVICE_PATH)
        .map_err(|_| error::driver_not_found())?;

    // Allocate enough space for the header + up to 64 port entries.
    const MAX_PORTS: usize = 64;
    let buf_size = mem::size_of::<GcomListPortsHeader>()
        + MAX_PORTS * mem::size_of::<GcomPortInfo>();
    let mut buffer = vec![0u8; buf_size];

    let bytes_returned = unsafe {
        device_ioctl(
            handle,
            IOCTL_GCOM_LIST_PORTS,
            std::ptr::null(),
            0,
            buffer.as_mut_ptr(),
            buf_size as u32,
        )
    };

    unsafe { let _ = CloseHandle(handle); }

    let bytes_returned = bytes_returned?;

    if (bytes_returned as usize) < mem::size_of::<GcomListPortsHeader>() {
        return Ok(vec![]);
    }

    let header = unsafe {
        &*(buffer.as_ptr() as *const GcomListPortsHeader)
    };

    let count = header.count as usize;
    let entries_ptr = unsafe {
        buffer.as_ptr().add(mem::size_of::<GcomListPortsHeader>()) as *const GcomPortInfo
    };

    let mut ports = Vec::with_capacity(count);
    for i in 0..count {
        let entry = unsafe { &*entries_ptr.add(i) };
        ports.push(PortInfoJs {
            port_number: entry.port_number,
            companion_index: entry.companion_index,
            com_side_open: entry.com_side_open != 0,
            companion_side_open: entry.companion_side_open != 0,
        });
    }

    Ok(ports)
}

/// Check whether the GCOM driver is installed and accessible.
#[napi]
pub fn is_driver_available() -> bool {
    match open_device_sync(CONTROL_DEVICE_PATH) {
        Ok(handle) => {
            unsafe { let _ = CloseHandle(handle); }
            true
        }
        Err(_) => false,
    }
}

/// Query the installed driver version.
///
/// Returns a version string like "0.1.0", or null if the driver
/// is not available.
#[napi]
pub fn get_driver_version() -> Result<Option<String>> {
    let handle = match open_device_sync(CONTROL_DEVICE_PATH) {
        Ok(h) => h,
        Err(_) => return Ok(None),
    };

    let mut version = GcomVersionInfo::default();

    let result = unsafe {
        device_ioctl(
            handle,
            IOCTL_GCOM_GET_VERSION,
            std::ptr::null(),
            0,
            &mut version as *mut _ as *mut u8,
            mem::size_of::<GcomVersionInfo>() as u32,
        )
    };

    unsafe { let _ = CloseHandle(handle); }

    match result {
        Ok(_) => Ok(Some(format!(
            "{}.{}.{}",
            version.major, version.minor, version.patch
        ))),
        Err(_) => Ok(None),
    }
}

/// Check that the driver's IOCTL protocol version is compatible
/// with this addon. Returns an error message if incompatible,
/// or null if compatible.
#[napi]
pub fn check_driver_compatibility() -> Result<Option<String>> {
    let handle = match open_device_sync(CONTROL_DEVICE_PATH) {
        Ok(h) => h,
        Err(_) => return Ok(Some("Driver not installed".to_string())),
    };

    let mut version = GcomVersionInfo::default();

    let result = unsafe {
        device_ioctl(
            handle,
            IOCTL_GCOM_GET_VERSION,
            std::ptr::null(),
            0,
            &mut version as *mut _ as *mut u8,
            mem::size_of::<GcomVersionInfo>() as u32,
        )
    };

    unsafe { let _ = CloseHandle(handle); }

    match result {
        Ok(_) => {
            if version.protocol_major != GCOM_PROTOCOL_VERSION_MAJOR {
                Ok(Some(format!(
                    "Driver protocol version {}.{} is incompatible with addon protocol version {}.{}. \
                     Please update the {} to match.",
                    version.protocol_major, version.protocol_minor,
                    GCOM_PROTOCOL_VERSION_MAJOR, GCOM_PROTOCOL_VERSION_MINOR,
                    if version.protocol_major < GCOM_PROTOCOL_VERSION_MAJOR {
                        "driver"
                    } else {
                        "addon (npm package)"
                    }
                )))
            } else {
                Ok(None) // Compatible
            }
        }
        Err(_) => Ok(Some("Failed to query driver version".to_string())),
    }
}
