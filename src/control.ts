import { createRequire } from "node:module";
import type { PortInfo } from "./types.js";

const require = createRequire(import.meta.url);

/**
 * The native control binding exposed by the Rust addon.
 * These map to IOCTLs on the \\.\VCOMControl device.
 */
export interface NativeControlBinding {
  /**
   * Send IOCTL_VCOM_CREATE_PORT to the control device.
   * Returns the assigned port number and companion index.
   */
  createPort(portNumber: number): { portNumber: number; companionIndex: number };

  /**
   * Send IOCTL_VCOM_DESTROY_PORT to the control device.
   */
  destroyPort(companionIndex: number): void;

  /**
   * Send IOCTL_VCOM_LIST_PORTS to the control device.
   */
  listPorts(): PortInfo[];

  /**
   * Check whether the VCOMControl device can be opened.
   */
  isDriverAvailable(): boolean;

  /**
   * Query the driver version string.
   */
  getDriverVersion(): string | null;
}

let _binding: NativeControlBinding | undefined;

/**
 * Lazily load the native addon.
 *
 * The addon is a platform-specific .node binary built by napi-rs.
 * This function handles the dynamic require and caches the result.
 */
function getBinding(): NativeControlBinding {
  if (_binding) return _binding;

  try {
    // napi-rs generates the binding with this structure.
    // The @napi-rs/cli `artifacts` command places the correct
    // platform binary alongside the package.
    // eslint-disable-next-line @typescript-eslint/no-require-imports
    const native = require("../addon/node-null.node");
    _binding = native as NativeControlBinding;
    return _binding;
  } catch (cause) {
    throw new Error(
      "Failed to load the node-null native addon. " +
        "Ensure the addon is built (`bun run build:addon`) and " +
        "the vcom driver is installed (`bun run install:driver`).",
      { cause },
    );
  }
}

/**
 * Create a new virtual COM port via the kernel driver.
 *
 * @param portNumber - Specific COM port number, or 0 for auto-assign.
 * @returns The assigned port number and companion device index.
 */
export function nativeCreatePort(portNumber = 0) {
  return getBinding().createPort(portNumber);
}

/**
 * Destroy a virtual COM port.
 */
export function nativeDestroyPort(companionIndex: number): void {
  getBinding().destroyPort(companionIndex);
}

/**
 * List all active virtual COM ports.
 */
export function nativeListPorts(): PortInfo[] {
  return getBinding().listPorts();
}

/**
 * Check if the vcom kernel driver is installed and accessible.
 */
export function nativeIsDriverAvailable(): boolean {
  try {
    return getBinding().isDriverAvailable();
  } catch {
    return false;
  }
}

/**
 * Get the installed driver version string, or null if unavailable.
 */
export function nativeGetDriverVersion(): string | null {
  try {
    return getBinding().getDriverVersion();
  } catch {
    return null;
  }
}
