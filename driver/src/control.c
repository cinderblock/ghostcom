/*
 * control.c — Control device for port management.
 *
 * The control device (\\.\VCOMControl) handles creation, destruction,
 * and enumeration of virtual COM port pairs.
 */

#include "driver.h"

/* ── Symbolic link for the control device ─────────────────────── */

static DECLARE_CONST_UNICODE_STRING(
    ControlDeviceName,
    L"\\Device\\VCOMControl"
);

static DECLARE_CONST_UNICODE_STRING(
    ControlSymLink,
    L"\\DosDevices\\VCOMControl"
);

/*
 * Module-level pointer to the FDO device context.
 * Set during VcomControlDeviceCreate and used by the IOCTL dispatch
 * to find the port table. This avoids fragile parent-object traversal.
 *
 * Safe because there is only ever one FDO instance (root-enumerated).
 */
static PVCOM_DEVICE_CTX g_DevCtx = NULL;


/* ── Create the control device ────────────────────────────────── */

NTSTATUS
VcomControlDeviceCreate(
    _In_ WDFDEVICE ParentDevice,
    _In_ PVCOM_DEVICE_CTX DevCtx
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
        &SDDL_DEVOBJ_SYS_ALL_ADM_ALL  /* Admin + SYSTEM access */
    );
    if (!controlInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Assign a device name. */
    status = WdfDeviceInitAssignName(controlInit, &ControlDeviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(controlInit);
        return status;
    }

    WdfDeviceInitSetIoType(controlInit, WdfDeviceIoBuffered);

    /* Create the control device. */
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = ParentDevice;

    WDFDEVICE controlDevice;
    status = WdfDeviceCreate(&controlInit, &attributes, &controlDevice);
    if (!NT_SUCCESS(status)) {
        /* controlInit is freed on failure */
        return status;
    }

    DevCtx->ControlDevice = controlDevice;

    /* Create a symbolic link so user-mode can open \\.\VCOMControl. */
    status = WdfDeviceCreateSymbolicLink(controlDevice, &ControlSymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "WdfDeviceCreateSymbolicLink failed: 0x%08X", status);
        return status;
    }

    /* ── Create an I/O queue for IOCTLs ─────────────────────── */

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = VcomControlIoDeviceControl;

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

    TraceEvents(0, 0, "Control device created: \\Device\\VCOMControl");

    return STATUS_SUCCESS;
}


/* ── Handle IOCTL_VCOM_CREATE_PORT ────────────────────────────── */

static VOID
VcomHandleCreatePort(
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PVCOM_CREATE_PORT_REQUEST input;
    PVCOM_CREATE_PORT_RESPONSE output;
    size_t inputLen, outputLen;
    PVCOM_PORT_PAIR portPair = NULL;

    /* Validate buffers. */
    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(VCOM_CREATE_PORT_REQUEST),
        (PVOID*)&input, &inputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(VCOM_CREATE_PORT_RESPONSE),
        (PVOID*)&output, &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Create the port pair (need the WDFDRIVER for creating control devices). */
    WDFDRIVER driver = WdfDeviceGetDriver(DevCtx->ControlDevice);
    status = VcomPortPairCreate(driver, DevCtx, input->PortNumber, &portPair);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    /* Fill response. */
    output->PortNumber = portPair->PortNumber;
    output->CompanionIndex = portPair->CompanionIndex;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                      sizeof(VCOM_CREATE_PORT_RESPONSE));

    TraceEvents(0, 0, "Created port pair: COM%lu ↔ VCOMCompanion%lu",
                portPair->PortNumber, portPair->CompanionIndex);
}


/* ── Handle IOCTL_VCOM_DESTROY_PORT ───────────────────────────── */

static VOID
VcomHandleDestroyPort(
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PVCOM_DESTROY_PORT_REQUEST input;
    size_t inputLen;

    status = WdfRequestRetrieveInputBuffer(
        Request, sizeof(VCOM_DESTROY_PORT_REQUEST),
        (PVOID*)&input, &inputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    /* Find the port pair by companion index. */
    PVCOM_PORT_PAIR portPair = NULL;

    WdfSpinLockAcquire(DevCtx->PortTableLock);
    for (ULONG i = 0; i < VCOM_MAX_PORTS; i++) {
        if (DevCtx->Ports[i] &&
            DevCtx->Ports[i]->CompanionIndex == input->CompanionIndex &&
            DevCtx->Ports[i]->Active)
        {
            portPair = DevCtx->Ports[i];
            break;
        }
    }
    WdfSpinLockRelease(DevCtx->PortTableLock);

    if (!portPair) {
        WdfRequestComplete(Request, STATUS_NOT_FOUND);
        return;
    }

    VcomPortPairDestroy(DevCtx, portPair);

    WdfRequestComplete(Request, STATUS_SUCCESS);

    TraceEvents(0, 0, "Destroyed port pair: companion index %lu",
                input->CompanionIndex);
}


/* ── Handle IOCTL_VCOM_LIST_PORTS ─────────────────────────────── */

static VOID
VcomHandleListPorts(
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PVOID outputBuf;
    size_t outputLen;

    status = WdfRequestRetrieveOutputBuffer(
        Request,
        sizeof(VCOM_LIST_PORTS_HEADER),
        &outputBuf,
        &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    PVCOM_LIST_PORTS_HEADER header = (PVCOM_LIST_PORTS_HEADER)outputBuf;
    PVCOM_PORT_INFO entries = (PVCOM_PORT_INFO)((PUCHAR)outputBuf +
                              sizeof(VCOM_LIST_PORTS_HEADER));

    ULONG maxEntries = (ULONG)(
        (outputLen - sizeof(VCOM_LIST_PORTS_HEADER)) / sizeof(VCOM_PORT_INFO)
    );

    ULONG count = 0;

    WdfSpinLockAcquire(DevCtx->PortTableLock);
    for (ULONG i = 0; i < VCOM_MAX_PORTS && count < maxEntries; i++) {
        if (DevCtx->Ports[i] && DevCtx->Ports[i]->Active) {
            PVCOM_PORT_PAIR pp = DevCtx->Ports[i];
            entries[count].PortNumber = pp->PortNumber;
            entries[count].CompanionIndex = pp->CompanionIndex;
            entries[count].ComSideOpen = pp->ComSideOpen ? 1 : 0;
            entries[count].CompanionSideOpen = pp->CompanionSideOpen ? 1 : 0;
            count++;
        }
    }
    WdfSpinLockRelease(DevCtx->PortTableLock);

    header->Count = count;

    size_t bytesWritten = sizeof(VCOM_LIST_PORTS_HEADER) +
                          count * sizeof(VCOM_PORT_INFO);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);
}


/* ── Handle IOCTL_VCOM_GET_VERSION ────────────────────────────── */

static VOID
VcomHandleGetVersion(
    _In_ WDFREQUEST Request
)
{
    NTSTATUS status;
    PVCOM_VERSION_INFO output;
    size_t outputLen;

    status = WdfRequestRetrieveOutputBuffer(
        Request, sizeof(VCOM_VERSION_INFO),
        (PVOID*)&output, &outputLen
    );
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    output->Major = VCOM_VERSION_MAJOR;
    output->Minor = VCOM_VERSION_MINOR;
    output->Patch = VCOM_VERSION_PATCH;
    output->Reserved = 0;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                      sizeof(VCOM_VERSION_INFO));
}


/* ── Control device IOCTL dispatch ────────────────────────────── */

VOID
VcomControlIoDeviceControl(
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

    PVCOM_DEVICE_CTX devCtx = g_DevCtx;

    switch (IoControlCode) {
    case IOCTL_VCOM_CREATE_PORT:
        VcomHandleCreatePort(devCtx, Request);
        break;

    case IOCTL_VCOM_DESTROY_PORT:
        VcomHandleDestroyPort(devCtx, Request);
        break;

    case IOCTL_VCOM_LIST_PORTS:
        VcomHandleListPorts(devCtx, Request);
        break;

    case IOCTL_VCOM_GET_VERSION:
        VcomHandleGetVersion(Request);
        break;

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        break;
    }
}
