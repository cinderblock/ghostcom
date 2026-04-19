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


/* ── Child list callback (stub for shadow PDOs) ──────────────── */

static NTSTATUS
GcomEvtChildListCreateDevice(
    _In_ WDFCHILDLIST ChildList,
    _In_ PWDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Id,
    _In_ PWDFDEVICE_INIT ChildInit
)
{
    UNREFERENCED_PARAMETER(ChildList);

    /* Extract port number from the identification description. */
    struct { WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header; ULONG PortNumber; } *desc =
        (void*)Id;
    ULONG portNumber = desc->PortNumber;

    /* Set hardware ID so the ghost-port INF can match. */
    UNICODE_STRING hwId;
    RtlInitUnicodeString(&hwId, L"GCOM\\COMPort");
    WdfPdoInitAddHardwareID(ChildInit, &hwId);

    /* Unique instance ID. */
    WCHAR instBuf[16];
    UNICODE_STRING instId;
    RtlStringCbPrintfW(instBuf, sizeof(instBuf), L"%lu", portNumber);
    RtlInitUnicodeString(&instId, instBuf);
    WdfPdoInitAssignInstanceID(ChildInit, &instId);

    /* Device description text. */
    WCHAR descBuf[80];
    UNICODE_STRING descStr, locStr;
    RtlStringCbPrintfW(descBuf, sizeof(descBuf),
                       L"GhostCOM Virtual Serial Port (COM%lu)", portNumber);
    RtlInitUnicodeString(&descStr, descBuf);
    RtlInitUnicodeString(&locStr, L"GhostCOM");
    WdfPdoInitAddDeviceText(ChildInit, &descStr, &locStr, 0x0409);
    WdfPdoInitSetDefaultLocale(ChildInit, 0x0409);

    /* Raw device in the Ports class — no function driver needed. */
    {
        /* GUID_DEVCLASS_PORTS = {4D36E978-E325-11CE-BFC1-08002BE10318} */
        GUID portsGuid = {0x4D36E978, 0xE325, 0x11CE,
                          {0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18}};
        WdfPdoInitAssignRawDevice(ChildInit, &portsGuid);
    }

    /* Create the PDO. */
    WDF_OBJECT_ATTRIBUTES attr;
    WDF_OBJECT_ATTRIBUTES_INIT(&attr);
    WDFDEVICE pdoDevice;
    NTSTATUS status = WdfDeviceCreate(&ChildInit, &attr, &pdoDevice);

    return status;
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

    /* ── Configure child list for shadow PDOs ────────────────── */
    {
        WDF_CHILD_LIST_CONFIG clc;
        WDF_CHILD_LIST_CONFIG_INIT(&clc, sizeof(ULONG),
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
