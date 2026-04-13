/*
 * driver.c — DriverEntry and EvtDeviceAdd for the node-null driver.
 *
 * This is a root-enumerated software-only KMDF driver. It creates
 * a single FDO that hosts all virtual COM port pairs and the control
 * device.
 */

#include "driver.h"

/* ── WPP Tracing (stub — replace with real WPP for production) ── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "node-null: " msg "\n", ##__VA_ARGS__))


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

    WDF_DRIVER_CONFIG_INIT(&config, VcomEvtDeviceAdd);

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
VcomEvtDeviceD0Entry(
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
VcomEvtDeviceD0Exit(
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
VcomEvtSelfManagedIoCleanup(
    _In_ WDFDEVICE Device
)
{
    PVCOM_DEVICE_CTX devCtx = VcomGetDeviceContext(Device);

    TraceEvents(0, 0, "SelfManagedIoCleanup — destroying all port pairs");

    /* Tear down all active port pairs on driver stop/unload. */
    for (ULONG i = 0; i < VCOM_MAX_PORTS; i++) {
        if (devCtx->Ports[i] && devCtx->Ports[i]->Active) {
            VcomPortPairDestroy(devCtx, devCtx->Ports[i]);
        }
    }
}


/* ── EvtDeviceAdd ─────────────────────────────────────────────── */

NTSTATUS
VcomEvtDeviceAdd(
    _In_ WDFDRIVER       Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;
    WDF_OBJECT_ATTRIBUTES devAttributes;
    WDFDEVICE device;
    PVCOM_DEVICE_CTX devCtx;

    UNREFERENCED_PARAMETER(Driver);

    TraceEvents(0, 0, "VcomEvtDeviceAdd");

    /* ── Configure the device init ──────────────────────────── */

    /*
     * Mark as exclusive — only one instance of this driver should
     * exist. The control device handles multiplexing.
     */
    WdfDeviceInitSetExclusive(DeviceInit, FALSE);

    /* Set a device name so we can create a control device symlink. */
    UNICODE_STRING deviceName;
    RtlInitUnicodeString(&deviceName, L"\\Device\\NodeNull");
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
        pnpCallbacks.EvtDeviceD0Entry = VcomEvtDeviceD0Entry;
        pnpCallbacks.EvtDeviceD0Exit = VcomEvtDeviceD0Exit;
        pnpCallbacks.EvtDeviceSelfManagedIoCleanup = VcomEvtSelfManagedIoCleanup;
        WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
    }

    /* ── Create the device ──────────────────────────────────── */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttributes, VCOM_DEVICE_CTX);
    devAttributes.EvtCleanupCallback = NULL;

    status = WdfDeviceCreate(&DeviceInit, &devAttributes, &device);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDeviceCreate failed: 0x%08X", status);
        return status;
    }

    /* ── Initialize device context ──────────────────────────── */

    devCtx = VcomGetDeviceContext(device);
    RtlZeroMemory(devCtx, sizeof(VCOM_DEVICE_CTX));
    devCtx->NextCompanionIndex = 0;

    /* Create spinlock for the port table. */
    WDF_OBJECT_ATTRIBUTES lockAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttributes);
    lockAttributes.ParentObject = device;

    status = WdfSpinLockCreate(&lockAttributes, &devCtx->PortTableLock);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfSpinLockCreate (PortTableLock) failed: 0x%08X", status);
        return status;
    }

    /* ── Create the control device ──────────────────────────── */

    status = VcomControlDeviceCreate(device, devCtx);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "VcomControlDeviceCreate failed: 0x%08X", status);
        return status;
    }

    TraceEvents(0, 0, "Driver initialized successfully");

    return STATUS_SUCCESS;
}
