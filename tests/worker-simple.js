// Simple worker to check if Bun Workers can use bun:ffi and send messages
import { dlopen, FFIType } from 'bun:ffi';

self.postMessage({ type: 'start' });

try {
  const { symbols: { GetLastError } } = dlopen('kernel32.dll', {
    GetLastError: { args: [], returns: FFIType.u32 },
  });
  const err = GetLastError();
  self.postMessage({ type: 'ffi_ok', err });
} catch (e) {
  self.postMessage({ type: 'ffi_error', msg: e.message });
}

self.postMessage({ type: 'done' });
