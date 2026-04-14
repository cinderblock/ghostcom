/*
 * control.c — Control device for port management.
 *
 * The control device (\\.\GCOMControl) handles creation, destruction,
 * and enumeration of virtual COM port pairs.
 */

#include "driver.h"

/* ── Tracing ──────────────────────────────────────────────────── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "ghostcom [CTRL]: " msg "\n", ##__VA_ARGS__))

/* ── Symbolic link for the control device ─────────────────────── */

static const WCHAR ControlDeviceNameBuf[] = L"\\Device\\GCOMControl";
static const WCHAR ControlSymLinkBuf[] = L"\\DosDevices\\GCOMControl";

/*
 * Module-level pointer to the FDO device context.
 * Set during GcomControlDeviceCreate and used by the IOCTL dispatch
 * to find the port table. This avoids fragile parent-object traversal.
 *
 * Safe because there is only ever one FDO instance (root-enumerated).
 */
static PGCOM_DEVICE_CTX g_DevCtx = NULL;


/* ── Create the control device ────────────────────────────────── */

NTSTATUS
GcomControlDeviceCreate(
    _In_ WDFDEVICE ParentDevice,
    _In_ PGCOM_DEVICE_CTX DevCtx
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT controlInit;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFQUEUE queue;

    TraceEvents(0, 0, "Creating control device");

    /*
     * Allocate a WDFDEVICE_INIT for the control device.
     * Control devices are "side" devices — they share the driver
     * but are not part of the PnP stack.
     */
    controlInit = WdfControlDeviceInitAllocate(
        WdfDeviceGetDriver(ParentDevice),
        &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_R_RES_R  /* Admins RWX, users read-only */
    );
    if (!controlInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Assign a device name. */
    UNICODE_STRING controlDeviceName;
    RtlInitUnicodeString(&controlDeviceName, ControlDeviceNameBuf);
    status = WdfDeviceInitAssignName(controlInit, &controlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    WdfDeviceInitSetIoType(controlInit, WdfDeviceIoBuffered);

    /* Create the control device.
     * Note: Control devices cannot have a ParentObject set via
     * WDF_OBJECT_ATTRIBUTES — they are parented to the driver
     * object implicitly. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

    WDFDEVICE controlDevice;
    status = WdfDeviceCreate(&controlInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        /* controlInit is freed on failure */
        return status;
    }

    DevCtx->ControlDevice = controlDevice;

    /* Create a symbolic link so user-mode can open \\.\GCOMControl. */
    UNICODE_STRING controlSymLink;
    RtlInitUnicodeString(&controlSymLink, ControlSymLinkBuf);
    status = WdfDeviceCreateSymbolicLink(controlDevice, &controlSymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDeviceCreateSymbolicLink failed: 0x%08X", status);
        return status;
    }

    /* ── Create an I/O queue for IOCTLs ─────────────────────── */

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = GcomControlIoDeviceControl;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = controlDevice;

    status = WdfIoQueueCreate(controlDevice, &queueConfig, &attributes, &queue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "Control queue create failed: 0x%08X", status);
        return status;
    }

    /* Store the device context pointer for IOCTL dispatch. */
    g_DevCtx = DevCtx;

    /* Finish initializing the control device (makes it accessible). */
    WdfControlFinishInitializing(controlDevice);

    TraceEvents(0, 0, "Control device created: \\Device\\GCOMControl");

    return STATUS_SUCCESS;
}


/* ── Handle IOCTL_GCOM_CREATE_PORT ────────────────────────────── */

static VOID
GcomHandleCreatePort(
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PGCOM_CREATE_PORT_REQUEST input;
    PGCOM_CREATE_PORT_RESPONSE output;
    size_t inputLen, outputLen;
    PGCOM_PORT_PAIR portPair = NULL;

    /* Validate buffers. */
    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(GCOM_CREATE_PORT_REQUEST),
        (PVOID*)&input, &inputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(GCOM_CREATE_PORT_RESPONSE),
        (PVOID*)&output, &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Create the port pair (need the WDFDRIVER for creating control devices). */
    WDFDRIVER driver = WdfDeviceGetDriver(DevCtx->ControlDevice);
    status = GcomPortPairCreate(driver, DevCtx, input->PortNumber, &portPair);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    /* Fill response. */
    output->PortNumber = portPair->PortNumber;
    output->CompanionIndex = portPair->CompanionIndex;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                      sizeof(GCOM_CREATE_PORT_RESPONSE));

    TraceEvents(0, 0, "Created port pair: COM%lu ↔ GCOM%lu",
                portPair->PortNumber, portPair->CompanionIndex);
}


/* ── Handle IOCTL_GCOM_DESTROY_PORT ───────────────────────────── */

static VOID
GcomHandleDestroyPort(
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PGCOM_DESTROY_PORT_REQUEST input;
    size_t inputLen;

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(GCOM_DESTROY_PORT_REQUEST),
        (PVOID*)&input, &inputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Find and atomically remove the port pair by companion index. */
    PGCOM_PORT_PAIR portPair = NULL;

    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (GCOM_PORT_IS_VALID(DevCtx->Ports[i]) &&
            DevCtx->Ports[i]->CompanionIndex == input->CompanionIndex &&
            DevCtx->Ports[i]->Active)
        {
            portPair = DevCtx->Ports[i];
            DevCtx->Ports[i] = NULL;
            DevCtx->PortCount--;
            break;
        }
    }
    WdfWaitLockRelease(DevCtx->PortTableLock);

    if (!portPair) {
        WdfRequestComplete(Request, STATUS_NOT_FOUND);
        return;
    }

    InterlockedExchange(&portPair->Active, FALSE);
    GcomPortPairDestroy(DevCtx, portPair);

    WdfRequestComplete(Request, STATUS_SUCCESS);

    TraceEvents(0, 0, "Destroyed port pair: companion index %lu",
                input->CompanionIndex);
}


/* ── Handle IOCTL_GCOM_LIST_PORTS ─────────────────────────────── */

static VOID
GcomHandleListPorts(
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PVOID outputBuf;
    size_t outputLen;

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(GCOM_LIST_PORTS_HEADER),
        &outputBuf,
        &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    PGCOM_LIST_PORTS_HEADER header = (PGCOM_LIST_PORTS_HEADER)outputBuf;
    PGCOM_PORT_INFO entries = (PGCOM_PORT_INFO)((PUCHAR)outputBuf +
                              sizeof(GCOM_LIST_PORTS_HEADER));

    ULONG maxEntries = (ULONG)(
        (outputLen - sizeof(GCOM_LIST_PORTS_HEADER)) / sizeof(GCOM_PORT_INFO)
    );

    ULONG count = 0;

    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
    for (ULONG i = 0; i < GCOM_MAX_PORTS && count < maxEntries; i++) {
        if (GCOM_PORT_IS_VALID(DevCtx->Ports[i]) && DevCtx->Ports[i]->Active) {
            PGCOM_PORT_PAIR pp = DevCtx->Ports[i];
            entries[count].PortNumber = pp->PortNumber;
            entries[count].CompanionIndex = pp->CompanionIndex;
            entries[count].ComSideOpen = pp->ComSideOpen ? 1 : 0;
            entries[count].CompanionSideOpen = pp->CompanionSideOpen ? 1 : 0;
            count++;
        }
    }
    WdfWaitLockRelease(DevCtx->PortTableLock);

    header->Count = count;

    size_t bytesWritten = sizeof(GCOM_LIST_PORTS_HEADER) +
                          count * sizeof(GCOM_PORT_INFO);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);
}


/* ── Handle IOCTL_GCOM_GET_VERSION ────────────────────────────── */

static VOID
GcomHandleGetVersion(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PGCOM_VERSION_INFO output;
    size_t outputLen;

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(GCOM_VERSION_INFO),
        (PVOID*)&output, &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    output->Major = GCOM_VERSION_MAJOR;
    output->Minor = GCOM_VERSION_MINOR;
    output->Patch = GCOM_VERSION_PATCH;
    output->ProtocolMajor = GCOM_PROTOCOL_VERSION_MAJOR;
    output->ProtocolMinor = GCOM_PROTOCOL_VERSION_MINOR;
    output->Reserved = 0;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                      sizeof(GCOM_VERSION_INFO));
}


/* ── Control device IOCTL dispatch ────────────────────────────── */

VOID
GcomControlIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
)
{
    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PGCOM_DEVICE_CTX devCtx = g_DevCtx;

    switch (IoControlCode) {
    case IOCTL_GCOM_CREATE_PORT:
        GcomHandleCreatePort(devCtx, Request);
        break;

    case IOCTL_GCOM_DESTROY_PORT:
        GcomHandleDestroyPort(devCtx, Request);
        break;

    case IOCTL_GCOM_LIST_PORTS:
        GcomHandleListPorts(devCtx, Request);
        break;

    case IOCTL_GCOM_GET_VERSION:
        GcomHandleGetVersion(Request);
        break;

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}
