# Known Bugs

## Data Path

- [ ] **Bidirectional data transfer hangs**: Writes complete successfully (companion → COM and COM → companion) but data sits in ring buffers and pending reads are never satisfied. `GcomDrainRingToReads` is called after writes but the read requests may not be queued yet, or the drain function has a synchronization issue with the `DataLock` spinlock.

- [ ] **onData callback receives null**: The native stream's ThreadsafeFunction sometimes passes null to the JS `onData` callback instead of a Buffer. May be a race in the read thread or an issue with how the Buffer is constructed from the overlapped ReadFile result.

## Driver Lifecycle

- [ ] **GCOM number doesn't match COM number**: Creating COM10 results in GCOM0 (or whatever the next auto-increment is). The companion device name should match: COM10 → GCOM10. The `NextCompanionIndex` counter auto-increments and never resets or reuses indices.

- [ ] **`sc stop` returns error 1052**: By design for PnP kernel drivers — `sc stop` doesn't work. Must use `devcon remove` instead. Should document this clearly and provide a helper command/script.

- [ ] **Duplicate PnP device nodes**: `devcon install` sometimes creates a second device node (e.g., `ROOT\SYSTEM\0001` and `ROOT\SYSTEM\0002`) when one already exists. The second fails with `STATUS_OBJECT_NAME_COLLISION` because `\\Device\\GCOMControl` already exists. Should check for existing devices before creating.

## Build & Deploy

- [ ] **Driver store caching**: `devcon install` doesn't update the driver store binary unless the INF `DriverVer` is bumped. Requires manually incrementing the version for every rebuild during development. Should automate version bumping in the build script.

- [ ] **`WdfDeviceInitSetExclusive(FALSE)` ineffective for control devices**: The WDF API doesn't clear the `DO_EXCLUSIVE` flag on control devices. Workaround: manually clear `DO_EXCLUSIVE` from the WDM `DEVICE_OBJECT` after `WdfDeviceCreate`. This is fragile and undocumented.

## Signal Path

- [ ] **Signal change notifications untested**: The `IOCTL_GCOM_WAIT_SIGNAL_CHANGE` inverted-call pattern and the signal watcher thread have not been tested end-to-end. Unknown whether `WaitCommEvent` from the COM side correctly triggers notifications on the companion side.

- [ ] **Control signal tapping untested**: Baud rate, DTR/RTS, parity, flow control changes from the COM side have not been verified to propagate through to the companion's signal events.

## Security

- [ ] **Control device allows all users to create/destroy ports**: The SDDL `WORLD_RW` on the control device lets any non-admin user create and destroy virtual COM ports. Consider whether create/destroy should require elevation while read-only queries (list, version) remain open to all users.
