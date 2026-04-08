/*
 * companion.c — Companion device: user-mode data I/O + signal wait.
 *
 * The companion device (\\.\VCOMCompanion<N>) is opened by the
 * node-null native addon. It is the "other end" of the null-modem
 * pair.
 *
 * Data written here flows into CompanionToCom ring buffer → COM reads.
 * Data readable here comes from ComToCompanion ring buffer → COM writes.
 *
 * The companion also supports:
 *   - IOCTL_VCOM_WAIT_SIGNAL_CHANGE — overlapped, inverted-call pattern
 *   - IOCTL_VCOM_GET_SIGNALS       — synchronous snapshot
 *   - IOCTL_VCOM_SET_SIGNALS       — set companion output signals
 */

#include "driver.h"

/* ── Tracing ──────────────────────────────────────────────────── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "node-null [COMP]: " msg "\n", ##__VA_ARGS__))


/* ── File create / close callbacks ────────────────────────────── */

VOID
VcomCompEvtFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PVCOM_PORT_DEVICE_CTX devCtx = VcomGetPortDeviceContext(Device);
    PVCOM_PORT_PAIR pp = devCtx->PortPair;

    /* Only allow one companion connection at a time. */
    if (InterlockedCompareExchange(&pp->CompanionSideOpen, 1, 0) != 0) {
        TraceEvents(0, 0, "VCOMCompanion%lu: rejected (already open)",
                    pp->CompanionIndex);
        WdfRequestComplete(Request, STATUS_SHARING_VIOLATION);
        return;
    }

    /* Set up the file context. */
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(FileObject);
    fileCtx->FileType = VcomFileTypeCompanion;
    fileCtx->PortPair = pp;

    TraceEvents(0, 0, "VCOMCompanion%lu opened", pp->CompanionIndex);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
VcomCompEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(FileObject);
    PVCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (pp) {
        InterlockedExchange(&pp->CompanionSideOpen, 0);
        TraceEvents(0, 0, "VCOMCompanion%lu closed", pp->CompanionIndex);
    }
}


/* ── Companion device creation ────────────────────────────────── */

NTSTATUS
VcomCompanionCreate(
    _In_ WDFDRIVER Driver,
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ PVCOM_PORT_PAIR PortPair
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT deviceInit;
    WDFDEVICE compDevice;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_FILEOBJECT_CONFIG fileConfig;

    UNREFERENCED_PARAMETER(DevCtx);

    /* ── Allocate and configure device init ──────────────────── */

    deviceInit = WdfControlDeviceInitAllocate(
        Driver,
        &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RWX  /* All users can R/W */
    );
    if (!deviceInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Assign the device name: \Device\VCOMCompanion<N> */
    WCHAR deviceNameBuf[64];
    UNICODE_STRING deviceName;
    RtlStringCbPrintfW(deviceNameBuf, sizeof(deviceNameBuf),
                       L"\\Device\\VCOMCompanion%lu", PortPair->CompanionIndex);
    RtlInitUnicodeString(&deviceName, deviceNameBuf);

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    /* Configure file object support. */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               VcomCompEvtFileCreate,
                               VcomCompEvtFileClose,
                               WDF_NO_EVENT_CALLBACK);

    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, VCOM_FILE_CTX);
    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, &fileAttributes);

    /* ── Create the device ──────────────────────────────────── */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VCOM_PORT_DEVICE_CTX);

    status = WdfDeviceCreate(&deviceInit, &attributes, &compDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Store port pair reference. */
    PVCOM_PORT_DEVICE_CTX portDevCtx = VcomGetPortDeviceContext(compDevice);
    portDevCtx->PortPair = PortPair;
    portDevCtx->IsComSide = FALSE;

    PortPair->CompanionDevice = compDevice;

    /* ── Create the default I/O queue ───────────────────────── */

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = VcomCompEvtRead;
    queueConfig.EvtIoWrite = VcomCompEvtWrite;
    queueConfig.EvtIoDeviceControl = VcomCompEvtIoctl;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = compDevice;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(compDevice, &queueConfig, &attributes, &defaultQueue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "Companion default queue create failed: 0x%08X", status);
        return status;
    }

    /* ── Create manual queues for pending I/O ───────────────── */

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = compDevice;

    /* Pending companion reads. */
    status = WdfIoQueueCreate(compDevice, &queueConfig, &attributes,
                               &PortPair->CompReadQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Pending companion writes. */
    status = WdfIoQueueCreate(compDevice, &queueConfig, &attributes,
                               &PortPair->CompWriteQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Pending signal wait requests (inverted call pattern). */
    status = WdfIoQueueCreate(compDevice, &queueConfig, &attributes,
                               &PortPair->SignalWaitQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ── Create symbolic link: \DosDevices\VCOMCompanion<N> ──── */

    WCHAR symLinkBuf[64];
    RtlStringCbPrintfW(symLinkBuf, sizeof(symLinkBuf),
                       L"\\DosDevices\\VCOMCompanion%lu",
                       PortPair->CompanionIndex);

    PortPair->CompSymLink.Buffer = (PWCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(symLinkBuf), VCOM_POOL_TAG);
    if (!PortPair->CompSymLink.Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(PortPair->CompSymLink.Buffer, symLinkBuf, sizeof(symLinkBuf));
    PortPair->CompSymLink.Length = (USHORT)(wcslen(symLinkBuf) * sizeof(WCHAR));
    PortPair->CompSymLink.MaximumLength = sizeof(symLinkBuf);

    status = WdfDeviceCreateSymbolicLink(compDevice, &PortPair->CompSymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "VCOMCompanion%lu symlink create failed: 0x%08X",
                    PortPair->CompanionIndex, status);
        ExFreePoolWithTag(PortPair->CompSymLink.Buffer, VCOM_POOL_TAG);
        PortPair->CompSymLink.Buffer = NULL;
        return status;
    }

    /* ── Finish initialization ──────────────────────────────── */

    WdfControlFinishInitializing(compDevice);

    TraceEvents(0, 0, "Companion device created: VCOMCompanion%lu",
                PortPair->CompanionIndex);

    return STATUS_SUCCESS;
}


/* ── Companion device destruction ─────────────────────────────── */

VOID
VcomCompanionDestroy(
    _In_ PVCOM_PORT_PAIR PortPair
)
{
    if (PortPair->CompSymLink.Buffer) {
        ExFreePoolWithTag(PortPair->CompSymLink.Buffer, VCOM_POOL_TAG);
        PortPair->CompSymLink.Buffer = NULL;
    }

    if (PortPair->CompanionDevice) {
        WdfObjectDelete(PortPair->CompanionDevice);
        PortPair->CompanionDevice = NULL;
    }
}


/* ── Companion-side Read ──────────────────────────────────────── */

VOID
VcomCompEvtRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
)
{
    UNREFERENCED_PARAMETER(Queue);

    WDFFILEOBJECT fileObj = WdfRequestGetFileObject(Request);
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(fileObj);
    PVCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (!pp || !pp->Active) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    /* Try to read from ComToCompanion ring buffer. */
    PVOID outputBuf;
    size_t outputLen;
    NTSTATUS st = WdfRequestRetrieveOutputBuffer(Request, 1, &outputBuf, &outputLen);
    if (!NT_SUCCESS(st)) {
        WdfRequestComplete(Request, st);
        return;
    }

    ULONG bytesRead = VcomRingRead(&pp->ComToCompanion,
                                    (PUCHAR)outputBuf,
                                    (ULONG)min(outputLen, Length));

    if (bytesRead > 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesRead);

        /* Unblock any pending COM-side writes. */
        if (pp->ComWriteQueue) {
            VcomDrainWritesToRing(&pp->ComToCompanion,
                                  pp->ComWriteQueue,
                                  pp->CompReadQueue);
        }
    } else {
        /* No data available — pend the request (overlapped ReadFile). */
        st = WdfRequestForwardToIoQueue(Request, pp->CompReadQueue);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── Companion-side Write ─────────────────────────────────────── */

VOID
VcomCompEvtWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
)
{
    UNREFERENCED_PARAMETER(Queue);

    WDFFILEOBJECT fileObj = WdfRequestGetFileObject(Request);
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(fileObj);
    PVCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (!pp || !pp->Active) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    PVOID inputBuf;
    size_t inputLen;
    NTSTATUS st = WdfRequestRetrieveInputBuffer(Request, 1, &inputBuf, &inputLen);
    if (!NT_SUCCESS(st)) {
        WdfRequestComplete(Request, st);
        return;
    }

    ULONG bytesWritten = VcomRingWrite(&pp->CompanionToCom,
                                        (const PUCHAR)inputBuf,
                                        (ULONG)min(inputLen, Length));

    if (bytesWritten > 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);

        /* Wake up pending COM-side reads. */
        if (pp->ComReadQueue) {
            VcomDrainRingToReads(&pp->CompanionToCom, pp->ComReadQueue);
        }

        /* Notify WaitCommEvent (EV_RXCHAR) on the COM side. */
        VcomCheckWaitMask(pp, SERIAL_EV_RXCHAR);
    } else {
        /* Ring buffer full — pend the write. */
        st = WdfRequestForwardToIoQueue(Request, pp->CompWriteQueue);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── Companion-side IOCTL handler ─────────────────────────────── */

VOID
VcomCompEvtIoctl(
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

    WDFFILEOBJECT fileObj = WdfRequestGetFileObject(Request);
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(fileObj);
    PVCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (!pp || !pp->Active) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    NTSTATUS status;

    switch (IoControlCode) {

    /* ── Wait for signal change (overlapped / inverted call) ── */

    case IOCTL_VCOM_WAIT_SIGNAL_CHANGE:
    {
        /*
         * Pend this request in the signal wait queue. It will be
         * completed by VcomSignalChanged() when the COM side
         * changes any configuration.
         *
         * The user-mode addon issues this overlapped and blocks
         * its signal-watcher thread on the overlapped event. When
         * the driver completes this request, the thread wakes up,
         * reads the signal state, and re-issues the IOCTL.
         */
        status = WdfRequestForwardToIoQueue(Request, pp->SignalWaitQueue);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Get current signal state ─────────────────────────── */

    case IOCTL_VCOM_GET_SIGNALS:
    {
        PVCOM_SIGNAL_STATE output;
        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(VCOM_SIGNAL_STATE),
            (PVOID*)&output, NULL
        );
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            *output = pp->SignalState;
            WdfSpinLockRelease(pp->SignalLock);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(VCOM_SIGNAL_STATE));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Set companion output signals ─────────────────────── */

    case IOCTL_VCOM_SET_SIGNALS:
    {
        PVCOM_SET_SIGNALS input;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(VCOM_SET_SIGNALS),
            (PVOID*)&input, NULL
        );
        if (NT_SUCCESS(status)) {
            BOOLEAN oldDtr = pp->CompDtr;
            BOOLEAN oldRts = pp->CompRts;

            pp->CompDtr = input->DtrState ? TRUE : FALSE;
            pp->CompRts = input->RtsState ? TRUE : FALSE;

            /*
             * If companion signals changed, notify the COM side's
             * WaitCommEvent. Through null-modem crossover:
             *   Companion DTR → COM sees DSR + DCD
             *   Companion RTS → COM sees CTS
             */
            ULONG events = 0;
            if (pp->CompRts != oldRts) events |= SERIAL_EV_CTS;
            if (pp->CompDtr != oldDtr) events |= SERIAL_EV_DSR | SERIAL_EV_RLSD;
            if (events) {
                VcomCheckWaitMask(pp, events);
            }

            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    default:
        TraceEvents(0, 0, "Unhandled companion IOCTL: 0x%08X", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}
