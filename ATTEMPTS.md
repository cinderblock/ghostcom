# ATTEMPTS.md

Append-only log of approaches tried, what failed, and why — so we don't
re-litigate the same dead ends. **Never rewrite history here.** If an
earlier note turns out to be wrong, add a new entry below correcting it.

Format: `## YYYY-MM-DD HH:MM — Problem / Question`, then `### Tried …`
and `### Result …` and `### Decision/Next`.

---

## 2026-04-18 — Test suite regresses from 23 visible tests to 10 tests

### Context
- Four test files: `e2e.test.ts`, `compat.test.ts`, `enumeration.test.ts`,
  `robustness.test.ts`. 23 tests total. Allure report on `http://host:4040`.
- `bun run test` → `scripts/run-tests.ts` runs each file sequentially.
- Two enumeration tests are expected failures until the PDO publication
  bug is fixed; the other 21 should pass.

### Tried: adding `afterAll(() => setTimeout(() => process.exit(0), 2000))` to every test file
- **Why**: The native addon keeps worker threads / N-API TSFNs alive
  after `shutdown()` / `close()`, which keeps bun's event loop alive past
  the end of the test suite. Bun's JUnit reporter only writes the XML on
  process exit, so without force-exit we lose the report.
- **Result**: Works for `e2e.test.ts` and `enumeration.test.ts`. Missing
  from `compat.test.ts` caused that file to time out silently (no XML).
- **Decision**: Add the same hook to `compat.test.ts`. **Do not remove** —
  there is no equivalent `process.exit` guardrail elsewhere.

### Tried: bumping per-file timeout in `run-tests.ts` from 120s → 180s
- **Why**: `e2e.test.ts` occasionally took >120s under Driver Verifier,
  getting killed before it could emit its XML.
- **Result**: Helped at the margin. Still not enough if a test file
  genuinely hangs on a driver handle.
- **Decision**: Keep 180s. Treat anything longer as a hang, not a
  slow test.

### Tried: killing orphan bun processes between test files
- **Why**: A test file that times out leaves a bun child holding
  `\\.\GhostCOMControl` via the native addon's open handle. The next
  file can't make a clean start because `native.listPorts()` blocks.
- **Approach**: After a test-file exit with non-zero status, enumerate
  `bun.exe` processes, exclude our own pid and the Allure server
  (port 4040), force-kill the rest.
- **Rejected placement**: initially called `killOrphanBun()` at the
  START of `runCleanup()` — this killed the **child** bun running
  `tests/cleanup.js` itself, since we can't distinguish it from an
  orphan. Moved the call to AFTER a non-zero test-file exit.
- **Decision**: Keep `killOrphanBun()` post-failure only.

### Tried: `process.exit(0)` at end of `tests/cleanup.js`
- **Why**: `cleanup.js` loads the addon to call `destroyPort()`. The
  addon's TSFNs keep bun alive after the script's top-level code
  finishes. The lingering bun holds the control-device handle,
  blocking the next test file's first `native.listPorts()`.
- **Result**: Fixed the cleanup-lingers-forever case. `cleanup.js` now
  exits promptly after destroying ports.
- **Decision**: Keep. Applies the same force-exit pattern as the test
  files themselves.

### Tried: full suite run after all three fixes in place
- **Result 1 (2026-04-19 00:25 UTC)**: Zero XMLs. `e2e.test.ts` hung at
  banner for 180s. Killed by script timeout.
- **Result 2 (2026-04-19 00:47 UTC)**: `bun run tests/cleanup.js`
  standalone works (exit 0, "destroyed 11"). Then
  `bun test tests/e2e.test.ts` alone — hangs at banner, 0 CPU.
- **Observation**: Running tests filtered by `-t "<pattern>"` works
  (e.g. `-t "ring boundary"` → 1 pass 1.3s; `-t "bidirectional"` matches
  the describe block and runs all 8 → all pass 10.5s). Full run
  without `-t` is flaky — sometimes hangs, sometimes passes 8/8.
- **Hypothesis**: Driver state gets into a bad mode across iterations.
  Restarting the FDO with `devcon restart Root\GhostCOM` clears it. The
  `sc stop GhostCOM` path fails with error 1052 ("not valid for this
  service") even though status says STOPPABLE.

---

## 2026-04-18 — Driver reports child PDO successfully, PnP never publishes it

### Context
- `WdfChildListAddOrUpdateChildDescriptionAsPresent` is called with a
  `GCOM_CHILD_ID { Header, PortNumber }` struct.
- `WDF_CHILD_LIST_CONFIG_INIT(&clc, sizeof(GCOM_CHILD_ID), callback)`
  — size includes header + PortNumber.
- Callback `GcomEvtChildListCreateDevice` returns STATUS_SUCCESS.
- Diagnostic values at `HKLM\SOFTWARE\GhostCOMDiag`:
  - `AddChild_HasList=1`
  - `AddChild_DescSize=8`
  - `AddChild_AddStatus=0x40000000` (STATUS_OBJECT_NAME_EXISTS —
    warning, not error; indicates we're re-adding a child that was
    never marked missing)
  - `AddChild_HasPhysDev=1`
  - `AddChild_InvalidatedRelations=1`
  - `Callback_Entry=<portNum>` (callback fires)
  - `Callback_AddHwId_Status=0`
  - `Callback_PdoCreate_Status=0`
  - `Callback_Interface_Status=0` (device interface registered OK)
  - `Callback_RegKey_Status=0xC0000184` (STATUS_INVALID_DEVICE_STATE —
    expected, can't open Device Parameters key pre-enumeration)
  - `Callback_Final_Status=0`
  - `Callback_Completed=<portNum>`
- `devcon findall "GCOM\*"` → no matching devices.
- `Get-PnpDevice -Class Ports` → no GCOM entries.
- `setupapi.dev.log` → no `GCOM\COMPort` install activity at all, not
  even an attempted match. PnP isn't even seeing the hardware IDs.

### Tried: `WdfPdoInitAssignRawDevice(ChildInit, &GUID_DEVCLASS_PORTS)`
- **Why**: Marks the PDO as raw (no function driver needed). Pair with
  `[GhostCOM_Port.NT.Services] AddService = ,0x00000002` (null service)
  in `ghostcom-port.inf`.
- **Result**: Callback succeeds, but PnP still doesn't enumerate the
  PDO. Hypothesis: raw devices bypass the class installer, so the PDO
  never gets published into the Ports class registry even though WDF
  created it.
- **Decision (pending)**: Try removing `WdfPdoInitAssignRawDevice` and
  let `ghostcom-port.inf` install the (null) function driver through
  the normal class-installer path.

### Tried: `IoInvalidateDeviceRelations(FdoPdo, BusRelations)` as belt-and-suspenders
- **Why**: Dynamic child-list `AddOrUpdateAsPresent` should auto-kick
  PnP, but some WDF versions require explicit invalidate.
- **Result**: Diagnostic confirms the call is made
  (`AddChild_InvalidatedRelations=1`). Doesn't change the outcome —
  PDO still not published.
- **Decision**: Keep it; it's cheap insurance. The bug is elsewhere.

### Not yet tried
- `WdfChildListBeginScan` / `EndScan` wrapping for the Add calls. The
  current code does a one-shot dynamic add, which the WDF docs say
  is valid — but worth trying if the current approach keeps failing.
- Calling `WdfDeviceSetPnpCapabilities(pdo, WdfTrue, …)` to mark the
  PDO as removable/silent/etc.
- Removing the FDO class from "System" and declaring it as a proper
  bus (class "HIDClass" won't work either — probably need
  `{4D36E97D-...}` System with explicit bus-extender device type).
- Running with !wdfkd / !wdftarget under WinDbg to see whether WDF
  thinks the child is "present" from PnP's viewpoint.

### Known side issue: destroy path never marks child as missing
- `GcomPortPairDestroy` never calls
  `WdfChildListUpdateChildDescriptionAsMissing`. So the child list
  accumulates entries across the driver's lifetime. Re-adding the
  same port number returns `STATUS_OBJECT_NAME_EXISTS`, and the
  callback is NOT re-invoked on the duplicate add (WDF short-circuits).
- This is likely a contributor to the enumeration tests being flaky,
  but it's not the root cause of "PDO never published": the FIRST
  add (no duplicate) also doesn't result in a published PDO.

### Tried: explicit `WDF_DEVICE_PNP_CAPABILITIES` on the PDO
- **Why**: Some WDF versions treat a PDO as "not physically present"
  if PnP capabilities aren't explicitly set. Set `Removable=TRUE`,
  `SurpriseRemovalOK=TRUE`, `NoDisplayInUI=FALSE`, and `Address`/
  `UINumber = portNumber`.
- **Result**: Diag confirms `Callback_PnpCapsSet=1`. No change —
  `DEVPKEY_Device_Children` on the FDO is still empty.
- **Decision**: Keep the call (it's correct hygiene) but it's not the
  bug.

### Tried: wrap AddChild in `WdfChildListBeginScan` / `EndScan`
- **Why**: Canonical KMDF pattern. EndScan internally triggers
  `IoInvalidateDeviceRelations(BusRelations)`, so PnP re-queries the
  FDO for its children.
- **Result**: Diag confirms `AddChild_EndScanDone=1`, PDO create fully
  succeeds (`Callback_PdoCreate_Status=0`, valid handle
  `Callback_PdoDeviceLow=0x55fc0ac8`, valid `PDEVICE_OBJECT`
  `Callback_PdoWdmLow=0xa98ac780`). FDO is `Status=OK, Problem=CM_PROB_NONE`.
  But `Get-PnpDeviceProperty -InstanceId ROOT\SYSTEM\0001 -KeyName
  DEVPKEY_Device_Children` returns empty string. `pnputil
  /enum-devicetree` shows zero children under ROOT\SYSTEM\0001.
  `setupapi.dev.log` still has zero GCOM\COMPort activity.
- **Decision**: Keep. The bug is not in the add path — it's that PnP
  either never queries BusRelations or WDF's response is empty.

### Current hypothesis: FDO has a `\Device\GhostCOM` name
- `GcomEvtDeviceAdd` calls `WdfDeviceInitAssignName(DeviceInit,
  L"\Device\GhostCOM")`. This was historical: before we had a
  separate control device, the FDO was opened directly by user-mode.
  It hasn't been needed since we switched to a dedicated
  `\Device\GCOMControl` control device.
- Named FDOs on bus drivers have been reported to cause PnP to skip
  BusRelations queries in some WDM paths. Bus-extender FDOs in the
  WDK samples (toaster, osrusbfx2, virtserial2) are all nameless.
- **To try next**: Remove `WdfDeviceInitAssignName` from the FDO.

---

## 2026-04-18 — `sc stop GhostCOM` fails with error 1052

### Context
- KMDF drivers using `EvtDriverDeviceAdd` get marked NOT_STOPPABLE by
  default. We added PnP power callbacks (`EvtDeviceD0Entry`,
  `EvtDeviceD0Exit`, `EvtDeviceSelfManagedIoCleanup`) to make the
  service stoppable.
- `sc query GhostCOM` now reports `STOPPABLE`.
- `sc stop GhostCOM` still fails: `[SC] ControlService FAILED 1052:
  The requested control is not valid for this service.`

### Tried: `sc stop GhostCOM`
- **Result**: Error 1052. Driver doesn't actually stop.

### Tried: `devcon restart Root\GhostCOM`
- **Result**: Success — the FDO restarts, child list is cleared,
  `listPorts()` returns `[]`. This is currently the only reliable
  way to reset driver state.
- **Decision**: Use `devcon restart Root\GhostCOM` between test runs
  if the driver is stuck. Don't rely on `sc stop`.

### Not yet tried
- Investigate what the `1052` really means. The KMDF docs say this
  can happen if `WdfControlDeviceInitAllocate`-created control devices
  aren't properly deleted before stop. Our
  `EvtSelfManagedIoCleanup` does `WdfObjectDelete(ControlDevice)`.
- The docs also mention that `WdfDeviceInitSetExclusive(false)` and
  a pending open handle from user-mode will block stop. We always
  have at least one handle open during tests.

---

## 2026-04-19 — Ruling out the EvtChildListCreateDevice async race

### Context
After removing `WdfDeviceInitAssignName` and setting
`FILE_DEVICE_BUS_EXTENDER`, the PDO still doesn't appear in
`CM_Get_Child(ROOT\SYSTEM\0001)` (CR_NO_SUCH_DEVNODE). Diagnostics show
the FDO stack is valid (`FdoWdm != FdoPhys`), child list is configured
(`FdoAdd_ChildListNonNull=1`), and the callback produces a valid PDO
(`Callback_PdoCreate_Status=0`, `Callback_PdoDeviceLow` non-zero,
`Callback_PdoWdmLow` non-zero).

### Tried: `WdfChildListRetrievePdo` immediately after `AddOrUpdate`
- **Why**: Verify whether WDF has committed the child to the list by
  the time we call `IoInvalidateDeviceRelations`. If RetrievePdo
  succeeds immediately, the child IS known to WDF.
- **Result**: `AddChild_RetrievePdoStatus=2` (NotYetCreated) — the
  `EvtChildListCreateDevice` callback runs **asynchronously** from
  `AddOrUpdate`. So the invalidate was racing the PDO creation.
- **Decision**: Must wait for the PDO to actually exist before
  invalidating.

### Tried: poll loop (`KeDelayExecutionThread`, 10×100ms) before invalidate
- **Why**: Guarantee `EvtChildListCreateDevice` has run and the PDO is
  committed before PnP queries BusRelations.
- **Result (v0.43)**: `AddChild_WaitAttempts=1` (one 100 ms wait),
  `AddChild_WaitFinalStatus=1` (Success), `AddChild_WaitPdoLow` matches
  `Callback_PdoDeviceLow` (same PDO). `AddChild_InvalidatedRelations=1`.
  **But `CM_Get_Child` still returns CR_NO_SUCH_DEVNODE.**
- **Decision**: The async race was real but not the only problem.
  Either PnP is not honoring our `IoInvalidateDeviceRelations`, or WDF
  is returning an empty BusRelations list despite having a valid PDO
  in the child list. Need to see what's happening at the IRP level.

### To try next: WDM preprocess for IRP_MN_QUERY_DEVICE_RELATIONS
- **Plan**: Install `WdfDeviceInitAssignWdmIrpPreprocessCallback` for
  `IRP_MJ_PNP` + `IRP_MN_QUERY_DEVICE_RELATIONS` on the FDO. Log the
  RelationType on entry; install an `IoSetCompletionRoutine` to log
  `IoStatus.Information->Count` on exit. Then:
  - If `Pnp_BusRel_SeenCount` never increments after
    `AddChild_InvalidatedRelations=1`, PnP is ignoring our invalidate.
  - If it increments but `Pnp_BusRel_CompCount=0`, WDF is dropping
    our children.
  - If it increments and `Pnp_BusRel_CompCount=1`, PnP is getting the
    PDO but not publishing it — the bug is further up the stack.

---

## 2026-04-19 — WDM preprocess callback proves BusRelations works; PnP drops the PDO anyway

### Context
Installed `WdfDeviceInitAssignWdmIrpPreprocessCallback` on the FDO for
`IRP_MJ_PNP` + `IRP_MN_QUERY_DEVICE_RELATIONS`. Both the preprocess
entry (logs the `DEVICE_RELATION_TYPE`) and a full completion routine
(logs `IoStatus.Status`, `Information->Count`, and the first PDO
pointer) run on every query.

### Result: the kernel side is working perfectly
After creating port 23:
- `Pnp_BusRel_SeenCount = 3` — PnP sent THREE `BusRelations` queries
  (FDO-start, WDF's internal invalidate after `AddOrUpdate`, and our
  explicit `IoInvalidateDeviceRelations`)
- `Pnp_BusRel_CompCount = 1` — WDF's handler returned 1 child PDO
- `Pnp_BusRel_CompStatus = 0` — STATUS_SUCCESS
- `Pnp_BusRel_FirstPdoLow == Callback_PdoWdmLow` — the `PDEVICE_OBJECT`
  PnP received is exactly the one our `EvtChildListCreateDevice` created.

A follow-up `pnputil /scan-devices` bumped `SeenCount` to 4 with the
same successful result — PnP is actively asking and receiving our PDO.

### But: PnP never creates a devnode for the PDO
- `HKLM\SYSTEM\CurrentControlSet\Enum\GCOM` does not exist.
- `CM_Locate_DevNodeW(ROOT\SYSTEM\0001)` → OK, `CM_Get_Child` →
  `CR_NO_SUCH_DEVNODE`.
- `Get-PnpDevice | Where InstanceId -like '*GCOM*'` → empty.
- `setupapi.dev.log` contains zero "Device Install (Hardware
  Initiated)" sections for GCOM\COMPort.
- `Kernel-PnP/Configuration` event log has nothing about our PDO.

So the IRP loop succeeds end-to-end but PnP silently drops the PDO
between BusRelations return and devnode creation. No error, no log,
no devnode.

### Completion-routine interference hypothesis
Removed the completion-routine / stack-copy path, leaving only entry
logging. Unchanged outcome — PDO still not published. So the
preprocess plumbing isn't the culprit.

### Current hypotheses to test
1. **INF match is failing silently.** `ghostcom-port.inf` (oem3.inf)
   declares `GCOM\COMPort` under `Class=Ports`. Maybe PnP can't pair
   it with the PDO. Need to preprocess `IRP_MN_QUERY_ID` on the child
   PDO and see exactly what Device ID / Hardware IDs we report back.
2. **`PortName` never gets set.** The PDO's Device Parameters key
   fails to open at PDO-creation time (`Callback_RegKey_Status =
   0xC0000184 STATUS_INVALID_DEVICE_STATE`). That's expected at
   creation time (MSDN: PLUGPLAY_REGKEY_DEVICE valid only post-
   install), but it means nothing sets `PortName` — which the Ports
   class installer needs to build the FriendlyName. If this blocks
   installation silently, that's our bug.
3. **PnP install-history cache.** After many driver-version iterations
   today, Windows may have cached "this device cannot be installed"
   verdicts in `HKLM\SYSTEM\Setup\Upgrade\PnP\Devices` or similar. A
   full registry scrub + reboot may be needed to prove this.

### Device Manager++ screenshot sidetrack
User sent a Device Manager++ screenshot showing "Driver Version
0.33.0.0" and "Windows cannot verify the digital signature for the
drivers." Investigation:
- Active INF is oem15.inf (v0.44.0.0), not 0.33; DM++ was showing
  cached state from an earlier install.
- Signature IS valid (`Get-AuthenticodeSignature` = Valid),
  `testsigning=Yes`, cert in both Trusted Root and Trusted Publisher.
- `Get-PnpDevice ROOT\SYSTEM\0001` reports `Status=Started`,
  `Problem=CM_PROB_NONE`.

Sidetrack cleanup: removed 12 stale `oem*.inf` entries (0.32–0.43)
from the DriverStore. Only oem3.inf (ghostcom-port.inf) and oem15.inf
(ghostcom.inf v0.44) remain.

---

## 2026-04-19 — **BREAKTHROUGH: PDO publishes. Smoking gun was BusQueryDeviceID failing with STATUS_NOT_SUPPORTED.**

### v0.45 intermediate regression — EvtChildListCreateDevice stopped firing
Between v0.44 and v0.45 the callback stopped firing entirely:
`Pnp_BusRel_SeenCount=3` (queries arriving) but zero `Callback_*`
entries. The v0.45 change that broke it: we removed the completion
routine from the FDO PnP preprocess (`IoCopyCurrentIrpStackLocationToNext`
+ `IoSetCompletionRoutine` were deleted from `GcomPnpPreprocessIrp`).
Restoring the completion routine in v0.46 immediately brought the
callback back. Hypothesis: without a completion routine, `WdfDeviceWdmDispatchPreprocessedIrp`
took a different code path that short-circuited WDF's BusRelations
child-list handler. **Do not remove that completion routine again.**

### Then the real bug surfaced
With the completion routine restored, the PDO creation callback
(`EvtChildListCreateDevice`) fired normally, and the PDO preprocess
callback we also installed for QUERY_ID, QUERY_CAPABILITIES,
QUERY_DEVICE_TEXT started logging the outcomes. This log IMMEDIATELY
identified the root cause:

```
Pdo_Query00130000_Status = 0xC00000BB  ← STATUS_NOT_SUPPORTED
                                         for QUERY_ID/BusQueryDeviceID
Pdo_Query00130001_Status = 0x0          ← HardwareIDs OK
Pdo_Query00130002_Status = 0x0          ← CompatibleIDs OK
Pdo_Query00130003_Status = 0x0          ← InstanceID OK
Pdo_Query00130005_Status = 0xC00000BB  ← ContainerID (optional) OK to fail
```

Without a DeviceID, PnP cannot create a devnode in Enum — it silently
drops the PDO. That was the entire "PDO never publishes" mystery.

### Fix (v0.47)
Added explicit call to `WdfPdoInitAssignDeviceID(ChildInit, L"GCOM\\COMPort")`
in `GcomEvtChildListCreateDevice`, immediately before
`WdfPdoInitAddHardwareID`. The KMDF docs claim the first hardware ID
is used as a DeviceID fallback, but in KMDF 1.33 that fallback does
NOT fire — the explicit assign is mandatory.

After the fix:
- `Pdo_Query00130000_Status=0` (success), `Pdo_Query00130000_InfoLow` is
  a non-NULL pointer to the DeviceID Unicode string.
- `HKLM\SYSTEM\CurrentControlSet\Enum\GCOM\COMPort\<instance>` exists
  with HardwareID, CompatibleIDs, FriendlyName, Driver, etc.
- `Get-PnpDevice -Class Ports` lists `"GhostCOM Virtual Serial Port (COM3)"`.
- Device Parameters key has `PortName = "COM3"` (allocated from ComDB by
  msports class installer).

### Open follow-up: COM-number mismatch
Two "COM numbers" are now in play:
1. Our bus driver's internal port number (e.g., 27) — used for the
   companion device `\Device\GCOMSerial27`, the `\DosDevices\COM27`
   symlink, and the SERIALCOMM entry `\Device\GCOMSerial27 → COM27`.
2. The Ports class installer's ComDB allocation (e.g., COM3) — written
   as `PortName` to the PDO's Device Parameters key.

Current enumeration tests expect `FriendlyName` to match
`(COM${portNumber})` where `portNumber` is what `native.createPort(0)`
returns (our internal number). The FriendlyName uses the class
installer's number. To reconcile:
- Easiest: after the PDO is published, the bus driver reads back the
  class installer's `PortName` and the addon returns THAT number as
  `portNumber`. Update the companion device's symlink + SERIALCOMM entry
  to match. Then tests see a single consistent number.
- Alternative: pre-populate `PortName` in the PDO's Device Parameters
  key before the class installer runs. `WdfDeviceOpenRegistryKey(PLUGPLAY_REGKEY_DEVICE)`
  fails with `STATUS_INVALID_DEVICE_STATE` during `EvtChildListCreateDevice`
  (PDO not yet enumerated), so the write needs to happen later — e.g.,
  after the wait loop in `GcomComPortCreate` sees
  `WdfChildListRetrieveDeviceSuccess`. The Device Parameters key IS
  created by then (it's what msports reads/writes), so
  `IoOpenDeviceRegistryKey` should work from that point on.

---

## Reference — facts to keep consistent

- Bun's JUnit reporter writes XML **only** on process exit. Timeouts
  → no XML. Hangs → no XML. Always need a force-exit guardrail.
- The native addon keeps N-API TSFNs alive. Any bun process that loads
  `addon/ghostcom.node` will not exit naturally.
- `\\.\GhostCOMControl` is the control-device symlink; at most one
  process should hold it at a time, and subsequent `CreateFile` on it
  blocks when a stuck bun still has the handle.
- Tests expect COM port numbers from `createPort(0)` (auto-assign),
  typically 10-16 in test runs.
- 23 expected tests: 8 e2e + 6 compat + 5 enumeration + 4 robustness.
- 2 enumeration tests are known failures until PDO publishes
  (`Get-PnpDevice -Class Ports` and `Get-CimInstance Win32_PnPEntity`
  with Ports ClassGuid).

---

## 2026-04-19 — v0.48: COM-number sync via PortName pre-write

**Status: 20/23 tests passing. All 5 enumeration tests green.**

The breakthrough from v0.47 got the PDO published in the Enum tree, but
the msports class installer was assigning its own ComDB COM number
(COM3, COM4, …) independent of our internal port number. Tests expect
the `FriendlyName` to contain `(COM<N>)` where `<N>` matches what
`createPort(0)` returned to the caller. Solution:

**Pre-write `PortName` to the PDO's Device Parameters key before msports
runs.** The Enum key is created by PnP right before the first
`IRP_MN_QUERY_ID` is dispatched, so the PDO's PnP preprocess callback
is the earliest hook that can write into it. msports' class installer
runs *later* (during INF match / START_DEVICE) and if `PortName` is
already set, it uses that value instead of allocating from ComDB.

### Implementation

Added `GCOM_PDO_CTX` with `PortNumber` and `PortNameWritten` fields,
declared via `WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCOM_PDO_CTX,
GcomGetPdoContext)`. In `GcomEvtChildListCreateDevice`, after
`WdfDeviceCreate` returns the PDO, attach the context and stash
`portNumber`. In `GcomPdoPnpPreprocessIrp` on the first `QUERY_ID`:

```c
PGCOM_PDO_CTX pdoCtx = GcomGetPdoContext(Device);
if (pdoCtx != NULL && !pdoCtx->PortNameWritten) {
    PDEVICE_OBJECT pdoWdm = WdfDeviceWdmGetDeviceObject(Device);
    HANDLE keyHandle = NULL;
    NTSTATUS st = IoOpenDeviceRegistryKey(
        pdoWdm, PLUGPLAY_REGKEY_DEVICE,
        KEY_SET_VALUE, &keyHandle);
    if (NT_SUCCESS(st) && keyHandle != NULL) {
        WCHAR portNameBuf[16];
        UNICODE_STRING valName;
        RtlInitUnicodeString(&valName, L"PortName");
        RtlStringCbPrintfW(portNameBuf, sizeof(portNameBuf),
                           L"COM%lu", pdoCtx->PortNumber);
        SIZE_T len = (wcslen(portNameBuf) + 1) * sizeof(WCHAR);
        ZwSetValueKey(keyHandle, &valName, 0, REG_SZ,
                      portNameBuf, (ULONG)len);
        pdoCtx->PortNameWritten = TRUE;
        ZwClose(keyHandle);
    }
}
```

Diagnostics confirm it works:
- `Callback_PdoCtxInit = 10` (port 10 context attached)
- `Pdo_IoOpenRegKey_Status = 0` (open succeeded)
- `Pdo_PortName_WriteStatus = 0` (ZwSetValueKey succeeded)
- `Pdo_PortName_Written = 10` (guard flag set)
- Device Manager shows: **GhostCOM Virtual Serial Port (COM10)**
- `Get-PnpDevice -Class Ports` returns FriendlyName matching port #.

### Gotcha — service marked-for-delete

After `uninstall-driver.ps1` runs `sc delete GhostCOM` while the driver
is still bound to a devnode, the service enters "marked for delete"
state (error 1072). Subsequent installs can't cleanly bind the new
binary — the old image stays in memory, new diagnostics don't appear.
**A reboot is required** to flush marked-for-delete before the v0.48
binary actually runs. Event ID 20003 with status 1072 in the System
log is the tell.

### Gotcha — second INF (ghostcom-port.inf)

`ghostcom-port.inf` (Class=Ports, HardwareID=GCOM\COMPort, null
service) must be installed separately. `install-driver.ps1` only
installs `ghostcom.inf`. Without the Ports-class INF the PDO ends up
with `CM_PROB_FAILED_INSTALL` and no Class/FriendlyName. Install via:

```
pnputil /add-driver C:\GhostCOM-src\driver\build\x64\Release\ghostcom-port.inf /install
```

### Remaining failures (3/23)

1. **e2e Test 5** — `closeIdx = -1`. Closing the COM side (CloseFile)
   does not deliver a `GCOM_CHANGED_COM_CLOSE` signal to the companion
   watcher. The watcher does receive `GCOM_CHANGED_COM_OPEN` on the
   initial open but not on reopen. Likely cause: the cleanup/close
   path doesn't call `GcomSignalChanged(pp, GCOM_CHANGED_COM_CLOSE)`.

2. **e2e Test 6** — `setSignals(true, true)` then `getSignals()`
   returns `dtrState = false`. The companion set/get round-trip via
   `IOCTL_GCOM_SET_SIGNALS` / `IOCTL_GCOM_GET_SIGNALS` is broken. The
   handlers look correct (writing/reading `pp->CompDtr` /
   `pp->CompRts`) — need to verify the native addon wraps these
   correctly.

3. **compat Test D** — `IOCTL_SERIAL_GET_BAUD_RATE` returns
   `nBytesReturned = 0` instead of 4. The driver handler calls
   `WdfRequestCompleteWithInformation(…, sizeof(SERIAL_BAUD_RATE))`
   which *should* set Information. Could be an addon FFI binding
   issue with the `lpBytesReturned` param.
