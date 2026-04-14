/*
 * companion.c — Companion device: user-mode data I/O + signal wait.
 *
 * The companion device (\\.\GCOM<N>) is opened by the
 * GhostCOM native addon. It is the "other end" of the null-modem
 * pair.
 *
 * Data written here flows into CompanionToCom ring buffer → COM reads.
 * Data readable here comes from ComToCompanion ring buffer → COM writes.
 *
 * The companion also supports:
 *   - IOCTL_GCOM_WAIT_SIGNAL_CHANGE — overlapped, inverted-call pattern
 *   - IOCTL_GCOM_GET_SIGNALS       — synchronous snapshot
 *   - IOCTL_GCOM_SET_SIGNALS       — set companion output signals
 */

#include "driver.h"

/* ── Tracing ──────────────────────────────────────────────────── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "ghostcom [COMP]: " msg "\n", ##__VA_ARGS__))


/* ── File create / close callbacks ────────────────────────────── */

VOID
GcomCompEvtFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PGCOM_PORT_DEVICE_CTX devCtx = GcomGetPortDeviceContext(Device);
    PGCOM_PORT_PAIR pp = devCtx->PortPair;

    /* Only allow one companion connection at a time. */
    if (InterlockedCompareExchange(&pp->CompanionSideOpen, 1, 0) != 0) {
        TraceEvents(0, 0, "GCOM%lu: rejected (already open)",
                    pp->CompanionIndex);
        WdfRequestComplete(Request, STATUS_SHARING_VIOLATION);
        return;
    }

    /* Set up the file context. */
    PGCOM_FILE_CTX fileCtx = GcomGetFileContext(FileObject);
    fileCtx->FileType = GcomFileTypeCompanion;
    fileCtx->PortPair = pp;

    TraceEvents(0, 0, "GCOM%lu opened", pp->CompanionIndex);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
GcomCompEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    PGCOM_FILE_CTX fileCtx = GcomGetFileContext(FileObject);
    PGCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (pp) {
        InterlockedExchange(&pp->CompanionSideOpen, 0);
        TraceEvents(0, 0, "GCOM%lu closed", pp->CompanionIndex);
    }
}


/* ── Companion device creation ────────────────────────────────── */

NTSTATUS
GcomCompanionCreate(
    _In_ WDFDRIVER Driver,
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ PGCOM_PORT_PAIR PortPair
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
        &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R  /* Admins RWX, users RW */
    );
    if (!deviceInit) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Assign the device name: \Device\GCOM<N> */
    WCHAR deviceNameBuf[64];
    UNICODE_STRING deviceName;
    RtlStringCbPrintfW(deviceNameBuf, sizeof(deviceNameBuf),
                       L"\\Device\\GCOM%lu", PortPair->CompanionIndex);
    RtlInitUnicodeString(&deviceName, deviceNameBuf);

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    /* Configure file object support. */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               GcomCompEvtFileCreate,
                               GcomCompEvtFileClose,
                               WDF_NO_EVENT_CALLBACK);

    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, GCOM_FILE_CTX);
    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, &fileAttributes);

    /* ── Create the device ──────────────────────────────────── */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, GCOM_PORT_DEVICE_CTX);

    status = WdfDeviceCreate(&deviceInit, &attributes, &compDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Store port pair reference. */
    PGCOM_PORT_DEVICE_CTX portDevCtx = GcomGetPortDeviceContext(compDevice);
    portDevCtx->PortPair = PortPair;
    portDevCtx->IsComSide = FALSE;

    PortPair->CompanionDevice = compDevice;

    /* ── Create the default I/O queue ───────────────────────── */

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = GcomCompEvtRead;
    queueConfig.EvtIoWrite = GcomCompEvtWrite;
    queueConfig.EvtIoDeviceControl = GcomCompEvtIoctl;

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

    /* ── Create symbolic link: \DosDevices\GCOM<N> ──── */

    WCHAR symLinkBuf[64];
    RtlStringCbPrintfW(symLinkBuf, sizeof(symLinkBuf),
                       L"\\DosDevices\\GCOM%lu",
                       PortPair->CompanionIndex);

    PortPair->CompSymLink.Buffer = (PWCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(symLinkBuf), GCOM_POOL_TAG);
    if (!PortPair->CompSymLink.Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(PortPair->CompSymLink.Buffer, symLinkBuf, sizeof(symLinkBuf));
    PortPair->CompSymLink.Length = (USHORT)(wcslen(symLinkBuf) * sizeof(WCHAR));
    PortPair->CompSymLink.MaximumLength = sizeof(symLinkBuf);

    status = WdfDeviceCreateSymbolicLink(compDevice, &PortPair->CompSymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "GCOM%lu symlink create failed: 0x%08X",
                    PortPair->CompanionIndex, status);
        ExFreePoolWithTag(PortPair->CompSymLink.Buffer, GCOM_POOL_TAG);
        PortPair->CompSymLink.Buffer = NULL;
        return status;
    }

    /* ── Finish initialization ──────────────────────────────── */

    WdfControlFinishInitializing(compDevice);

    TraceEvents(0, 0, "Companion device created: GCOM%lu",
                PortPair->CompanionIndex);

    return STATUS_SUCCESS;
}


/* ── Companion device destruction ─────────────────────────────── */

VOID
GcomCompanionDestroy(
    _In_ PGCOM_PORT_PAIR PortPair
)
{
    if (PortPair->CompSymLink.Buffer) {
        ExFreePoolWithTag(PortPair->CompSymLink.Buffer, GCOM_POOL_TAG);
        PortPair->CompSymLink.Buffer = NULL;
    }

    if (PortPair->CompanionDevice) {
        WdfObjectDelete(PortPair->CompanionDevice);
        PortPair->CompanionDevice = NULL;
    }
}


/* ── Companion-side Read ──────────────────────────────────────── */

VOID
GcomCompEvtRead(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
)
{
    UNREFERENCED_PARAMETER(Queue);

    WDFFILEOBJECT fileObj = WdfRequestGetFileObject(Request);
    PGCOM_FILE_CTX fileCtx = GcomGetFileContext(fileObj);
    PGCOM_PORT_PAIR pp = fileCtx->PortPair;

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

    WdfSpinLockAcquire(pp->DataLock);

    ULONG bytesRead = GcomRingRead(&pp->ComToCompanion,
                                    (PUCHAR)outputBuf,
                                    (ULONG)min(outputLen, Length));

    if (bytesRead > 0) {
        /* Unblock any pending COM-side writes. */
        if (pp->ComWriteQueue) {
            GcomDrainWritesToRing(&pp->ComToCompanion,
                                  pp->ComWriteQueue,
                                  pp->CompReadQueue);
        }
        WdfSpinLockRelease(pp->DataLock);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesRead);
    } else {
        /* No data available — pend the request (overlapped ReadFile). */
        st = WdfRequestForwardToIoQueue(Request, pp->CompReadQueue);
        WdfSpinLockRelease(pp->DataLock);

        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── Companion-side Write ─────────────────────────────────────── */

VOID
GcomCompEvtWrite(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     Length
)
{
    UNREFERENCED_PARAMETER(Queue);

    WDFFILEOBJECT fileObj = WdfRequestGetFileObject(Request);
    PGCOM_FILE_CTX fileCtx = GcomGetFileContext(fileObj);
    PGCOM_PORT_PAIR pp = fileCtx->PortPair;

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

    WdfSpinLockAcquire(pp->DataLock);

    ULONG bytesWritten = GcomRingWrite(&pp->CompanionToCom,
                                        (const PUCHAR)inputBuf,
                                        (ULONG)min(inputLen, Length));

    if (bytesWritten > 0) {
        /* Wake up pending COM-side reads. */
        if (pp->ComReadQueue) {
            GcomDrainRingToReads(&pp->CompanionToCom, pp->ComReadQueue);
        }
        WdfSpinLockRelease(pp->DataLock);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);

        /* Notify WaitCommEvent (EV_RXCHAR) on the COM side. */
        GcomCheckWaitMask(pp, SERIAL_EV_RXCHAR);
    } else {
        /* Ring buffer full — pend the write. */
        st = WdfRequestForwardToIoQueue(Request, pp->CompWriteQueue);
        WdfSpinLockRelease(pp->DataLock);

        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── Companion-side IOCTL handler ─────────────────────────────── */

VOID
GcomCompEvtIoctl(
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
    PGCOM_FILE_CTX fileCtx = GcomGetFileContext(fileObj);
    PGCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (!pp || !pp->Active) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_CONNECTED);
        return;
    }

    NTSTATUS status;

    switch (IoControlCode) {

    /* ── Wait for signal change (overlapped / inverted call) ── */

    case IOCTL_GCOM_WAIT_SIGNAL_CHANGE:
    {
        /*
         * Pend this request in the signal wait queue. It will be
         * completed by GcomSignalChanged() when the COM side
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

    case IOCTL_GCOM_GET_SIGNALS:
    {
        PGCOM_SIGNAL_STATE output;
        status = WdfRequestRetrieveOutputBuffer(
            Request, sizeof(GCOM_SIGNAL_STATE),
            (PVOID*)&output, NULL
        );
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            *output = pp->SignalState;
            WdfSpinLockRelease(pp->SignalLock);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(GCOM_SIGNAL_STATE));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Set companion output signals ─────────────────────── */

    case IOCTL_GCOM_SET_SIGNALS:
    {
        PGCOM_SET_SIGNALS input;
        status = WdfRequestRetrieveInputBuffer(
            Request, sizeof(GCOM_SET_SIGNALS),
            (PVOID*)&input, NULL
        );
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            BOOLEAN oldDtr = pp->CompDtr;
            BOOLEAN oldRts = pp->CompRts;

            pp->CompDtr = input->DtrState ? TRUE : FALSE;
            pp->CompRts = input->RtsState ? TRUE : FALSE;
            WdfSpinLockRelease(pp->SignalLock);

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
                GcomCheckWaitMask(pp, events);
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
