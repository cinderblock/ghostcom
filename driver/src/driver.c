/*
 * driver.c — DriverEntry and EvtDeviceAdd for the GhostCOM driver.
 *
 * This is a root-enumerated software-only KMDF driver. It creates
 * a single FDO that hosts all virtual COM port pairs and the control
 * device.
 */

#include "driver.h"

/* ── WPP Tracing (stub — replace with real WPP for production) ── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "ghostcom: " msg "\n", ##__VA_ARGS__))

/*
 * Diagnostic helper — writes a DWORD status to
 *   HKLM\SOFTWARE\GhostCOMDiag\<ValueName>
 * so we can inspect driver state from user-mode without needing a
 * kernel debugger attached.
 */
VOID
GcomDiagWriteStatus(_In_ PCWSTR ValueName, _In_ ULONG Status)
{
    UNICODE_STRING keyPath;
    RtlInitUnicodeString(&keyPath, L"\\Registry\\Machine\\SOFTWARE\\GhostCOMDiag");

    OBJECT_ATTRIBUTES keyAttr;
    InitializeObjectAttributes(&keyAttr, &keyPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);
    HANDLE keyHandle;
    NTSTATUS st = ZwCreateKey(&keyHandle, KEY_SET_VALUE, &keyAttr, 0, NULL,
                              REG_OPTION_NON_VOLATILE, NULL);
    if (!NT_SUCCESS(st)) return;

    UNICODE_STRING valName;
    RtlInitUnicodeString(&valName, ValueName);
    ZwSetValueKey(keyHandle, &valName, 0, REG_DWORD, &Status, sizeof(Status));
    ZwClose(keyHandle);
}


/* ── Child list callback — creates the Ports-class PDO ────────── */

/*
 * We define these GUIDs locally (with explicit data init) rather than
 * relying on INITGUID + <initguid.h>, because ntddser.h already does a
 * DEFINE_GUID(GUID_DEVINTERFACE_COMPORT, ...) that — under INITGUID —
 * clashes with the one we'd pull in via <initguid.h> in this TU.
 */
static const GUID gGuidDevclassPorts =
    { 0x4D36E978, 0xE325, 0x11CE,
      { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };

static const GUID gGuidDevinterfaceComport =
    { 0x86E0D1E0, 0x8089, 0x11D0,
      { 0x9C, 0xE4, 0x08, 0x00, 0x3E, 0x30, 0x1F, 0x73 } };

static NTSTATUS
GcomEvtChildListCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Id,
    _In_ PWDFDEVICE_INIT ChildInit
)
{
    UNREFERENCED_PARAMETER(ChildList);

    NTSTATUS status;

    /* Extract port number from the identification description. */
    PGCOM_CHILD_ID desc = (PGCOM_CHILD_ID)Id;
    ULONG portNumber = desc->PortNumber;

    TraceEvents(0, 0, "ChildListCreateDevice: creating PDO for COM%lu", portNumber);
    GcomDiagWriteStatus(L"Callback_Entry", portNumber);

    /* Hardware ID — matches ghostcom-port.inf. */
    UNICODE_STRING hwId;
    RtlInitUnicodeString(&hwId, L"GCOM\\COMPort");
    status = WdfPdoInitAddHardwareID(ChildInit, &hwId);
    GcomDiagWriteStatus(L"Callback_AddHwId_Status", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfPdoInitAddHardwareID failed 0x%08X",
                    portNumber, status);
        return status;
    }

    /* Compatible ID so the INF-less install path also works. */
    UNICODE_STRING compatId;
    RtlInitUnicodeString(&compatId, L"GCOM\\COMPort");
    WdfPdoInitAddCompatibleID(ChildInit, &compatId);

    /* Unique instance ID (the port number). */
    WCHAR instBuf[16];
    UNICODE_STRING instId;
    RtlStringCbPrintfW(instBuf, sizeof(instBuf), L"%lu", portNumber);
    RtlInitUnicodeString(&instId, instBuf);
    status = WdfPdoInitAssignInstanceID(ChildInit, &instId);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfPdoInitAssignInstanceID failed 0x%08X",
                    portNumber, status);
        return status;
    }

    /* Device description text — what shows up as DeviceDesc and is used
     * by the Ports class installer to build the FriendlyName. */
    WCHAR descBuf[80];
    UNICODE_STRING descStr, locStr;
    RtlStringCbPrintfW(descBuf, sizeof(descBuf),
                       L"GhostCOM Virtual Serial Port (COM%lu)", portNumber);
    RtlInitUnicodeString(&descStr, descBuf);
    RtlInitUnicodeString(&locStr, L"GhostCOM");
    status = WdfPdoInitAddDeviceText(ChildInit, &descStr, &locStr, 0x0409);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfPdoInitAddDeviceText failed 0x%08X",
                    portNumber, status);
        return status;
    }
    WdfPdoInitSetDefaultLocale(ChildInit, 0x0409);

    /* Raw device in the Ports class — no function driver needed. */
    status = WdfPdoInitAssignRawDevice(ChildInit, &gGuidDevclassPorts);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfPdoInitAssignRawDevice failed 0x%08X",
                    portNumber, status);
        return status;
    }

    /* Allow user-mode (PowerShell / Get-PnpDevice) to open file handles
     * on the PDO. D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU): SYSTEM and
     * Admins get all access, Users get read. */
    UNICODE_STRING sddl;
    RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;BU)");
    status = WdfDeviceInitAssignSDDLString(ChildInit, &sddl);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfDeviceInitAssignSDDLString failed 0x%08X",
                    portNumber, status);
        return status;
    }

    /* Create the PDO. */
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    WDFDEVICE pdoDevice;
    status = WdfDeviceCreate(&ChildInit, &attr, &pdoDevice);
    GcomDiagWriteStatus(L"Callback_PdoCreate_Status", (ULONG)status);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu: WdfDeviceCreate (PDO) failed 0x%08X",
                    portNumber, status);
        return status;
    }

    /* Register the serial-port device interface. This is what makes
     * the PDO discoverable via GUID_DEVINTERFACE_COMPORT and therefore
     * visible to Get-CimInstance Win32_PnPEntity and SetupDi*. */
    NTSTATUS ifStatus = WdfDeviceCreateDeviceInterface(
        pdoDevice, &gGuidDevinterfaceComport, NULL);
    GcomDiagWriteStatus(L"Callback_Interface_Status", (ULONG)ifStatus);
    if (!NT_SUCCESS(ifStatus)) {
        TraceEvents(0, 0,
            "COM%lu: WdfDeviceCreateDeviceInterface(COMPORT) failed 0x%08X",
            portNumber, ifStatus);
        /* Non-fatal — PDO still exists. */
    }

    /* Write PortName = "COM<N>" to the device's Device Parameters key.
     * Standard convention used by every Windows serial-port stack; the
     * Ports class installer reads it to form the FriendlyName
     * ("<DeviceDesc> (COM<N>)") and MSSerial/serenum also read it. */
    {
        WDFKEY devParamsKey = NULL;
        NTSTATUS rkSt = WdfDeviceOpenRegistryKey(
            pdoDevice, PLUGPLAY_REGKEY_DEVICE,
            KEY_SET_VALUE, WDF_NO_OBJECT_ATTRIBUTES, &devParamsKey);
        GcomDiagWriteStatus(L"Callback_RegKey_Status", (ULONG)rkSt);
        if (NT_SUCCESS(rkSt) && devParamsKey != NULL) {
            UNICODE_STRING portNameValName;
            RtlInitUnicodeString(&portNameValName, L"PortName");
            WCHAR portNameBuf[16];
            UNICODE_STRING portNameValData;
            RtlStringCbPrintfW(portNameBuf, sizeof(portNameBuf),
                               L"COM%lu", portNumber);
            RtlInitUnicodeString(&portNameValData, portNameBuf);
            NTSTATUS wrSt = WdfRegistryAssignUnicodeString(
                devParamsKey, &portNameValName, &portNameValData);
            GcomDiagWriteStatus(L"Callback_PortName_Status", (ULONG)wrSt);
            if (!NT_SUCCESS(wrSt)) {
                TraceEvents(0, 0,
                    "COM%lu: WdfRegistryAssignUnicodeString(PortName) failed 0x%08X",
                    portNumber, wrSt);
            }
            WdfRegistryClose(devParamsKey);
        } else {
            TraceEvents(0, 0,
                "COM%lu: WdfDeviceOpenRegistryKey(Device params) failed 0x%08X",
                portNumber, rkSt);
        }
    }

    TraceEvents(0, 0, "ChildListCreateDevice: PDO created for COM%lu OK",
                portNumber);
    GcomDiagWriteStatus(L"Callback_Final_Status", (ULONG)STATUS_SUCCESS);
    GcomDiagWriteStatus(L"Callback_Completed", portNumber);

    return STATUS_SUCCESS;
}


/* ── DriverEntry ──────────────────────────────────────────────── */

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    WDF_DRIVER_CONFIG config;

    TraceEvents(0, 0, "DriverEntry");

    WDF_DRIVER_CONFIG_INIT(&config, GcomEvtDeviceAdd);

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE
    );

    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDriverCreate failed: 0x%08X", status);
        return status;
    }

    return STATUS_SUCCESS;
}


/* ── PnP Power Callbacks ──────────────────────────────────────── */

/*
 * These callbacks make the device stoppable, so the driver service
 * can be stopped and updated without requiring a reboot.
 */

NTSTATUS
GcomEvtDeviceD0Entry(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(PreviousState);
    TraceEvents(0, 0, "Device entering D0 (powered on)");
    return STATUS_SUCCESS;
}

NTSTATUS
GcomEvtDeviceD0Exit(
    _In_ WDFDEVICE Device,
    _In_ WDF_POWER_DEVICE_STATE TargetState
)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(TargetState);
    TraceEvents(0, 0, "Device exiting D0 (powering down)");
    return STATUS_SUCCESS;
}

VOID
GcomEvtSelfManagedIoCleanup(
    _In_ WDFDEVICE Device
)
{
    PGCOM_DEVICE_CTX devCtx = GcomGetDeviceContext(Device);

    TraceEvents(0, 0, "SelfManagedIoCleanup — destroying all port pairs");

    /* Tear down all active port pairs on driver stop/unload. */
    WdfWaitLockAcquire(devCtx->PortTableLock, NULL);
    PGCOM_PORT_PAIR toDestroy[GCOM_MAX_PORTS];
    ULONG destroyCount = 0;
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (GCOM_PORT_IS_VALID(devCtx->Ports[i]) && devCtx->Ports[i]->Active) {
            toDestroy[destroyCount++] = devCtx->Ports[i];
            devCtx->Ports[i] = NULL;
            devCtx->PortCount--;
        }
    }
    WdfWaitLockRelease(devCtx->PortTableLock);

    for (ULONG i = 0; i < destroyCount; i++) {
        InterlockedExchange(&toDestroy[i]->Active, FALSE);
        GcomPortPairDestroy(devCtx, toDestroy[i]);
    }

    /*
     * Destroy the control device so the driver can fully unload.
     *
     * WDF control devices are parented to the WDFDRIVER, not the FDO,
     * so they survive FDO removal. If we don't delete them here, the
     * driver image stays loaded and can't be updated without a reboot.
     */
    if (devCtx->ControlDevice) {
        TraceEvents(0, 0, "Destroying control device");
        GcomControlDeviceInvalidate();
        WdfObjectDelete(devCtx->ControlDevice);
        devCtx->ControlDevice = NULL;
    }
}


/* ── EvtDeviceAdd ─────────────────────────────────────────────── */

NTSTATUS
GcomEvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES devAttributes;
    WDFDEVICE device;
    PGCOM_DEVICE_CTX devCtx;

    UNREFERENCED_PARAMETER(Driver);

    TraceEvents(0, 0, "GcomEvtDeviceAdd");

    /* ── Configure the device init ──────────────────────────── */

    /*
     * Mark as exclusive — only one instance of this driver should
     * exist. The control device handles multiplexing.
     */
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    /* Set a device name so we can create a control device symlink. */
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, L"\\Device\\GhostCOM");
    status = WdfDeviceInitAssignName(DeviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDeviceInitAssignName failed: 0x%08X", status);
        return status;
    }

    /*
     * Allow user-mode to open file handles on the device (for the
     * control device and companion devices).
     */
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    /*
     * Register PnP power callbacks so the device is stoppable.
     * Without these, WDF marks the service as NOT_STOPPABLE and
     * the driver can't be stopped/updated without a reboot.
     */
    {
        WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
        WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
        pnpCallbacks.EvtDeviceD0Entry = GcomEvtDeviceD0Entry;
        pnpCallbacks.EvtDeviceD0Exit = GcomEvtDeviceD0Exit;
        pnpCallbacks.EvtDeviceSelfManagedIoCleanup = GcomEvtSelfManagedIoCleanup;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
    }

    /* ── Configure child list for shadow PDOs ────────────────── *
     * The size passed here is the TOTAL size of the identification
     * description (header + driver-defined fields), not just the
     * driver-defined fields. WDF memcpys exactly this many bytes
     * when a driver later calls WdfChildListAddOrUpdateChildDescriptionAsPresent
     * — passing sizeof(ULONG) here clips everything after the first 4
     * bytes of the HEADER, which results in garbage identifications and
     * the PDO never actually showing up in PnP. */
    {
        WDF_CHILD_LIST_CONFIG clc;
        WDF_CHILD_LIST_CONFIG_INIT(&clc, sizeof(GCOM_CHILD_ID),
                                    GcomEvtChildListCreateDevice);
        WdfFdoInitSetDefaultChildListConfig(DeviceInit, &clc,
                                             WDF_NO_OBJECT_ATTRIBUTES);
    }

    /* ── Create the device ──────────────────────────────────── */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttributes, GCOM_DEVICE_CTX);
    devAttributes.EvtCleanupCallback = NULL;

    status = WdfDeviceCreate(&DeviceInit, &devAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    /* ── Initialize device context ──────────────────────────── */

    devCtx = GcomGetDeviceContext(device);
    RtlZeroMemory(devCtx, sizeof(GCOM_DEVICE_CTX));
    devCtx->FdoDevice = device;
    devCtx->NextCompanionIndex = 0;

    /* Create spinlock for the port table. */
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;

    status = WdfWaitLockCreate(&lockAttributes, &devCtx->PortTableLock);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfSpinLockCreate (PortTableLock) failed: 0x%08X", status);
        return status;
    }

    /* ── Create the control device ──────────────────────────── */

    status = GcomControlDeviceCreate(device, devCtx);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "GcomControlDeviceCreate failed: 0x%08X", status);
        return status;
    }

    TraceEvents(0, 0, "Driver initialized successfully");

    return STATUS_SUCCESS;
}
