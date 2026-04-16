# Known Bugs

## Data Path

- [x] **Bidirectional data transfer hangs — root cause identified and fixed**: The
  `onData` ThreadsafeFunction used `ErrorStrategy::CalleeHandled`, which caused the
  JS callback to receive `(null, Buffer)` instead of `(Buffer)`. The TypeScript
  `VirtualPortStream` constructor called `this.push(null)` with the null first
  argument, immediately closing the readable stream and permanently stopping all
  data flow. Fixed: changed to `ErrorStrategy::Fatal` so the callback receives the
  Buffer directly.

  Secondary fix: `onSignalChange` had the same null-argument bug. Fixed.

  Secondary fix: write errors were silently swallowed (passed as the second
  `Ok(Some(errStr))` argument instead of `Err(napiErr)`). Fixed.

  Secondary fix: read errors were only printed to stderr; now delivered to the
  registered JS error callback. Fixed.

- [ ] **`companion → COM` data path requires driver rebuild (ReadIntervalTimeout
  fix)**: `serialport` (and .NET `SerialPort`) use a two-phase read strategy:
  first a 1-byte `ReadFileEx` (APC-based), then a `ReadFile` with
  `ReadIntervalTimeout = MAXDWORD` to drain any remaining bytes. Our driver pends
  the MAXDWORD read when the ring is empty instead of completing it with 0 bytes
  immediately. This leaves the library's worker thread stuck in
  `WaitForSingleObject(INFINITE)` until more data arrives.
  
  **Fix** (already applied to `driver/src/comport.c`): in `GcomComEvtRead`, when
  `ReadIntervalTimeout == MAXDWORD` and both total-timeout fields are 0, complete
  the request immediately with 0 bytes.
  
  **Status**: fix is in source; requires driver rebuild + reinstall.

- [ ] **`serialport` + Bun: `uv_async_send` from native threads doesn't wake Bun's
  event loop**: The driver itself is correct (verified by testing with Python,
  PowerShell, and Bun FFI). The problem is specific to using `node-serialport` with
  Bun:

  1. `serialport` creates a native Windows thread with `CreateThread`.
  2. That thread issues `ReadFileEx` (APC-based) and enters `SleepEx(INFINITE, TRUE)`.
  3. The driver correctly completes the IRP and queues the APC for the ReadThread.
  4. `SleepEx` returns `WAIT_IO_COMPLETION` (192) — the APC IS delivered.
  5. The `ReadIOCompletion` C++ APC callback fires.
  6. It calls `uv_async_send` to notify the Bun event loop.
  7. **Bun never processes the notification** — the `data` event never fires.

  The root cause is that `uv_async_send` called from a raw `CreateThread` native
  thread does not wake Bun's event loop, even though it works fine in Node.js.
  Bun's libuv compatibility layer does not appear to handle `uv_async_send` from
  non-Bun-managed threads. This is a Bun bug; the driver is not at fault.

  **Minimal repro**: a native addon that calls `uv_async_send` from a `CreateThread`
  thread. In Node.js the callback fires; in Bun it does not.

  **Workaround for tests**: use Bun FFI (`bun:ffi`) with `ReadFile` + overlapped
  event (not `ReadFileEx` + APC). This works correctly because the completion event
  is signalled by the kernel (not via `uv_async_send`).

  **What does work in Bun**: `napi_call_threadsafe_function` / NAPI TsfN (used by
  our Rust addon) — these take a different code path in Bun and work from any thread.

## Driver Lifecycle

- [x] **GCOM number doesn't match COM number**: Creating COM10 used to result in
  GCOM0 (auto-incremented companion index). Fixed: `portpair.c` now uses
  `portNumber` as the companion index, so COM10 ↔ GCOM10.

- [ ] **Stale symlink after crash**: If the driver process is killed without a clean
  `destroyPort()`, the `\DosDevices\COM<N>` symlink persists in the kernel object
  namespace even though the underlying device is gone. Subsequent `createPort(N)`
  calls fail with `STATUS_OBJECT_NAME_COLLISION`. Workaround: use a different port
  number, or restart the GhostCOM driver service.

  **Fix**: in `GcomPortPairCreate`, when a specific `RequestedPortNumber` is given,
  check whether the DOS symlink already exists before proceeding (the same check
  `GcomFindFreePortNumber` already does for auto-assigned ports).

- [ ] **`sc stop` returns error 1052**: By design for PnP kernel drivers — `sc stop`
  doesn't work. Must use `devcon remove` instead. Should document this clearly and
  provide a helper command/script.

- [ ] **Duplicate PnP device nodes**: `devcon install` sometimes creates a second
  device node (e.g., `ROOT\SYSTEM\0001` and `ROOT\SYSTEM\0002`) when one already
  exists. The second fails with `STATUS_OBJECT_NAME_COLLISION` because
  `\\Device\\GCOMControl` already exists. Should check for existing devices before
  creating.

## Build & Deploy

- [ ] **Driver store caching**: `devcon install` doesn't update the driver store
  binary unless the INF `DriverVer` is bumped. Requires manually incrementing the
  version for every rebuild during development. Should automate version bumping in
  the build script.

- [ ] **`WdfDeviceInitSetExclusive(FALSE)` ineffective for control devices**: The WDF
  API doesn't clear the `DO_EXCLUSIVE` flag on control devices. Workaround: manually
  clear `DO_EXCLUSIVE` from the WDM `DEVICE_OBJECT` after `WdfDeviceCreate`. This is
  fragile and undocumented.

- [ ] **`fs.openSync` fails with `EUNKNOWN` on virtual COM ports in Bun**: Bun's
  `fs.openSync(path, 'r+')` uses `FILE_SHARE_NONE` (`dwShareMode = 0`) when opening
  device files. This fails on our WDF control devices for an undetermined reason
  (returns an unknown Win32 error). Using `FILE_SHARE_READ | FILE_SHARE_WRITE` via
  direct FFI call works. The `serialport` library also uses sharing flags and opens
  successfully.

## Bun Test Runner

- [ ] **Parallel test execution conflicts**: `bun test tests/` runs both test files
  in parallel worker processes. Both test suites interact with the same driver
  (same COM port namespace), so parallel execution causes port conflicts and test
  hangs. Run them sequentially: `bun run test` (the package.json script handles
  cleanup and sequencing). Individual files still work: `bun test tests/e2e.test.ts`
  and `bun test tests/compat.test.ts`.

## Signal Path

- [x] **Signal change notifications**: Verified end-to-end. `IOCTL_GCOM_WAIT_SIGNAL_CHANGE`,
  DTR/RTS changes, baud rate changes, and COM_OPEN/COM_CLOSE all propagate correctly
  to the companion's signal watcher thread.

- [x] **`onSignalChange` callback received null instead of signal state**: Same
  `ErrorStrategy::CalleeHandled` bug as `onData`. Fixed.

- [x] **Signal watcher thread hangs on shutdown**: `SignalWatcher::stop()` didn't
  call `cancel_io()` before joining the signal watcher thread, causing an indefinite
  hang if no signal change arrived. Fixed: `stop()` now calls `cancel_io(handle)`
  first.

## Write Path

- [ ] **Partial writes silently truncate data**: `GcomRingWrite` can write fewer
  bytes than requested (when the ring is nearly full). The driver returns
  `STATUS_SUCCESS` with `bytesWritten < inputLen`. The Rust `do_write` function
  does not check whether all bytes were written; it returns `Ok(())` on the first
  successful `WriteFile` regardless of byte count. For payloads larger than
  `GCOM_RING_BUFFER_SIZE - 1` bytes (65535 bytes with a 64 KB ring), the last
  bytes are silently dropped.

  **Impact**: writing exactly `GCOM_RING_BUFFER_SIZE` (64 KB) loses 1 byte.

  **Fix**: in `do_write`, check `bytes_written < data.len()` after the WriteFile
  completes and retry with the remaining slice.

## Security

- [ ] **Control device allows all users to create/destroy ports**: The SDDL
  `WORLD_RW` on the control device lets any non-admin user create and destroy
  virtual COM ports. Consider whether create/destroy should require elevation while
  read-only queries (list, version) remain open to all users.
