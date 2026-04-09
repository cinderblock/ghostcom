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
