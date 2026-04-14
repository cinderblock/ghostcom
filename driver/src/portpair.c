/*
 * portpair.c — Port pair lifecycle and data transfer coordination.
 *
 * A port pair consists of a COM device (opened by external apps) and
 * a companion device (opened by the GhostCOM addon). This file
 * manages creation, destruction, data transfer between ring buffers
 * and I/O queues, and signal change notifications.
 */

#include "driver.h"

/* ── Tracing ──────────────────────────────────────────────────── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "ghostcom [PAIR]: " msg "\n", ##__VA_ARGS__))

/* ── Find an unused COM port number ───────────────────────────── */

ULONG
GcomFindFreePortNumber(
    _In_ PGCOM_DEVICE_CTX DevCtx
)
{
    /*
     * Scan from COM10 upward, skipping any numbers already in use
     * by our driver or by the system (check the SERIALCOMM registry).
     */
    for (ULONG candidate = 10; candidate < 256; candidate++) {
        BOOLEAN inUse = FALSE;

        /* Check our own ports. */
        for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
            if (DevCtx->Ports[i] &&
                DevCtx->Ports[i]->Active &&
                DevCtx->Ports[i]->PortNumber == candidate)
            {
                inUse = TRUE;
                break;
            }
        }
        if (inUse) continue;

        /*
         * Check the system's SERIALCOMM device map.
         * Key: HKLM\HARDWARE\DEVICEMAP\SERIALCOMM
         * Values are REG_SZ like "COM1", "COM3", etc.
         *
         * We check if any value equals "COM<candidate>".
         */
        WCHAR comName[16];
        UNICODE_STRING comNameStr;
        RtlStringCbPrintfW(comName, sizeof(comName), L"COM%lu", candidate);
        RtlInitUnicodeString(&comNameStr, comName);

        /* Try to create the DOS device name — if it already exists,
         * the creation of the symbolic link will fail later, so this
         * is a best-effort check. */
        WCHAR dosName[64];
        RtlStringCbPrintfW(dosName, sizeof(dosName),
                           L"\\DosDevices\\COM%lu", candidate);
        UNICODE_STRING dosNameStr;
        RtlInitUnicodeString(&dosNameStr, dosName);

        OBJECT_ATTRIBUTES objAttr;
        InitializeObjectAttributes(&objAttr, &dosNameStr,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        HANDLE linkHandle;
        NTSTATUS st = ZwOpenSymbolicLinkObject(&linkHandle,
                                               SYMBOLIC_LINK_QUERY,
                                               &objAttr);
        if (NT_SUCCESS(st)) {
            ZwClose(linkHandle);
            continue;  /* This COM number is taken. */
        }

        return candidate;
    }

    return 0;  /* No free port found */
}


/* ── Create a port pair ───────────────────────────────────────── */

NTSTATUS
GcomPortPairCreate(
    _In_  WDFDRIVER Driver,
    _In_  PGCOM_DEVICE_CTX DevCtx,
    _In_  ULONG RequestedPortNumber,
    _Out_ PGCOM_PORT_PAIR* OutPortPair
)
{
    NTSTATUS status;
    PGCOM_PORT_PAIR pp = NULL;
    ULONG slotIndex;

    *OutPortPair = NULL;

    /* Find a free slot in the port table. */
    WdfSpinLockAcquire(DevCtx->PortTableLock);

    slotIndex = GCOM_MAX_PORTS;
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (DevCtx->Ports[i] == NULL) {
            slotIndex = i;
            break;
        }
    }

    if (slotIndex == GCOM_MAX_PORTS) {
        WdfSpinLockRelease(DevCtx->PortTableLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Determine port number. */
    ULONG portNumber;
    if (RequestedPortNumber != 0) {
        /* Check if the requested number is already in use. */
        for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
            if (DevCtx->Ports[i] &&
                DevCtx->Ports[i]->Active &&
                DevCtx->Ports[i]->PortNumber == RequestedPortNumber)
            {
                WdfSpinLockRelease(DevCtx->PortTableLock);
                return STATUS_OBJECT_NAME_COLLISION;
            }
        }
        portNumber = RequestedPortNumber;
    } else {
        portNumber = GcomFindFreePortNumber(DevCtx);
        if (portNumber == 0) {
            WdfSpinLockRelease(DevCtx->PortTableLock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    ULONG companionIndex = (ULONG)InterlockedIncrement(&DevCtx->NextCompanionIndex) - 1;

    WdfSpinLockRelease(DevCtx->PortTableLock);

    /* Allocate the port pair structure. */
    pp = (PGCOM_PORT_PAIR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(GCOM_PORT_PAIR),
        GCOM_POOL_TAG
    );
    if (!pp) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pp, sizeof(GCOM_PORT_PAIR));
    pp->PortNumber = portNumber;
    pp->CompanionIndex = companionIndex;
    pp->Active = TRUE;
    pp->RefCount = 1;

    /* Initialize ring buffers. */
    status = GcomRingInit(&pp->ComToCompanion, GCOM_RING_BUFFER_SIZE);
    if (!NT_SUCCESS(status)) {
        ExFreePoolWithTag(pp, GCOM_POOL_TAG);
        return status;
    }

    status = GcomRingInit(&pp->CompanionToCom, GCOM_RING_BUFFER_SIZE);
    if (!NT_SUCCESS(status)) {
        GcomRingFree(&pp->ComToCompanion);
        ExFreePoolWithTag(pp, GCOM_POOL_TAG);
        return status;
    }

    /* Initialize default signal state. */
    WDF_OBJECT_ATTRIBUTES lockAttr;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);

    status = WdfSpinLockCreate(&lockAttr, &pp->SignalLock);
    if (!NT_SUCCESS(status)) {
        GcomRingFree(&pp->CompanionToCom);
        GcomRingFree(&pp->ComToCompanion);
        ExFreePoolWithTag(pp, GCOM_POOL_TAG);
        return status;
    }

    pp->SignalState.BaudRate = 9600;
    pp->SignalState.DataBits = 8;
    pp->SignalState.StopBits = STOP_BIT_1;
    pp->SignalState.Parity = NO_PARITY;
    pp->CompDtr = TRUE;   /* Companion asserts DTR by default */
    pp->CompRts = TRUE;   /* Companion asserts RTS by default */

    /* Default timeouts. */
    pp->Timeouts.ReadIntervalTimeout = 0;
    pp->Timeouts.ReadTotalTimeoutMultiplier = 0;
    pp->Timeouts.ReadTotalTimeoutConstant = 0;
    pp->Timeouts.WriteTotalTimeoutMultiplier = 0;
    pp->Timeouts.WriteTotalTimeoutConstant = 0;

    /* Store in the table BEFORE creating devices, so that if device
     * creation callbacks reference the table, the entry exists. */
    WdfSpinLockAcquire(DevCtx->PortTableLock);
    DevCtx->Ports[slotIndex] = pp;
    DevCtx->PortCount++;
    WdfSpinLockRelease(DevCtx->PortTableLock);

    /* ── Create the COM port device (\\.\COM<N>) ────────────── */

    status = GcomComPortCreate(Driver, DevCtx, pp, portNumber);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "GcomComPortCreate failed: 0x%08X", status);
        goto fail_cleanup;
    }

    /* ── Create the companion device (\\.\GCOM<N>) ─── */

    status = GcomCompanionCreate(Driver, DevCtx, pp);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "GcomCompanionCreate failed: 0x%08X", status);
        GcomComPortDestroy(pp);
        goto fail_cleanup;
    }

    *OutPortPair = pp;

    TraceEvents(0, 0, "Port pair created: COM%lu ↔ GCOM%lu (slot %lu)",
                portNumber, companionIndex, slotIndex);

    return STATUS_SUCCESS;

fail_cleanup:
    /* Remove from table. */
    WdfSpinLockAcquire(DevCtx->PortTableLock);
    DevCtx->Ports[slotIndex] = NULL;
    DevCtx->PortCount--;
    WdfSpinLockRelease(DevCtx->PortTableLock);

    GcomRingFree(&pp->CompanionToCom);
    GcomRingFree(&pp->ComToCompanion);
    ExFreePoolWithTag(pp, GCOM_POOL_TAG);

    return status;
}


/* ── Destroy a port pair ──────────────────────────────────────── */

VOID
GcomPortPairDestroy(
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ PGCOM_PORT_PAIR PortPair
)
{
    PortPair->Active = FALSE;

    /* Remove from the port table. */
    WdfSpinLockAcquire(DevCtx->PortTableLock);
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (DevCtx->Ports[i] == PortPair) {
            DevCtx->Ports[i] = NULL;
            DevCtx->PortCount--;
            break;
        }
    }
    WdfSpinLockRelease(DevCtx->PortTableLock);

    /* Cancel all pending I/O. */
    if (PortPair->ComReadQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->ComReadQueue);
    }
    if (PortPair->ComWriteQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->ComWriteQueue);
    }
    if (PortPair->CompReadQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->CompReadQueue);
    }
    if (PortPair->CompWriteQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->CompWriteQueue);
    }
    if (PortPair->SignalWaitQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->SignalWaitQueue);
    }
    if (PortPair->WaitMaskQueue) {
        WdfIoQueuePurgeSynchronously(PortPair->WaitMaskQueue);
    }

    /* Free ring buffers. */
    GcomRingFree(&PortPair->ComToCompanion);
    GcomRingFree(&PortPair->CompanionToCom);

    /* Delete symbolic links and devices. */
    GcomComPortDestroy(PortPair);
    GcomCompanionDestroy(PortPair);

    TraceEvents(0, 0, "Port pair destroyed: COM%lu", PortPair->PortNumber);

    ExFreePoolWithTag(PortPair, GCOM_POOL_TAG);
}


/* ── Signal change notification ───────────────────────────────── */

VOID
GcomSignalChanged(
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG ChangedBits
)
{
    /*
     * Increment the sequence number and set the changed bits.
     * Then complete any pending WAIT_SIGNAL_CHANGE requests on the
     * companion side.
     */
    WdfSpinLockAcquire(PortPair->SignalLock);

    PortPair->SignalState.SequenceNumber++;
    PortPair->SignalState.ChangedMask |= ChangedBits;

    GCOM_SIGNAL_STATE snapshot = PortPair->SignalState;

    WdfSpinLockRelease(PortPair->SignalLock);

    /* Complete all pending signal wait IRPs. */
    if (PortPair->SignalWaitQueue) {
        WDFREQUEST waitReq;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(
                PortPair->SignalWaitQueue, &waitReq)))
        {
            PGCOM_SIGNAL_STATE output;
            size_t outputLen;
            NTSTATUS st = WdfRequestRetrieveOutputBuffer(
                waitReq,
                sizeof(GCOM_SIGNAL_STATE),
                (PVOID*)&output,
                &outputLen
            );
            if (NT_SUCCESS(st)) {
                *output = snapshot;
                WdfRequestCompleteWithInformation(
                    waitReq, STATUS_SUCCESS,
                    sizeof(GCOM_SIGNAL_STATE)
                );
            } else {
                WdfRequestComplete(waitReq, st);
            }
        }
    }

    /* Clear the changed mask after notifying all waiters. */
    WdfSpinLockAcquire(PortPair->SignalLock);
    PortPair->SignalState.ChangedMask = 0;
    WdfSpinLockRelease(PortPair->SignalLock);
}


/* ── Drain ring buffer to pending read IRPs ───────────────────── */

VOID
GcomDrainRingToReads(
    _In_ PGCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE ReadQueue
)
{
    WDFREQUEST readReq;

    while (!GcomRingIsEmpty(Ring) &&
           NT_SUCCESS(WdfIoQueueRetrieveNextRequest(ReadQueue, &readReq)))
    {
        PVOID readBuf;
        size_t readBufLen;
        NTSTATUS st = WdfRequestRetrieveOutputBuffer(
            readReq, 1, &readBuf, &readBufLen
        );
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(readReq, st);
            continue;
        }

        ULONG bytesRead = GcomRingRead(Ring, (PUCHAR)readBuf, (ULONG)readBufLen);
        if (bytesRead > 0) {
            WdfRequestCompleteWithInformation(readReq, STATUS_SUCCESS, bytesRead);
        } else {
            /* No data — re-queue the request. */
            WdfRequestForwardToIoQueue(readReq, ReadQueue);
            break;
        }
    }
}


/* ── Drain pending write IRPs into ring buffer ────────────────── */

VOID
GcomDrainWritesToRing(
    _In_ PGCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE WriteQueue,
    _In_ WDFQUEUE PeerReadQueue
)
{
    WDFREQUEST writeReq;

    while (GcomRingWriteAvailable(Ring) > 0 &&
           NT_SUCCESS(WdfIoQueueRetrieveNextRequest(WriteQueue, &writeReq)))
    {
        PVOID writeBuf;
        size_t writeBufLen;
        NTSTATUS st = WdfRequestRetrieveInputBuffer(
            writeReq, 1, &writeBuf, &writeBufLen
        );
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(writeReq, st);
            continue;
        }

        ULONG bytesWritten = GcomRingWrite(Ring, (const PUCHAR)writeBuf,
                                           (ULONG)writeBufLen);
        if (bytesWritten > 0) {
            WdfRequestCompleteWithInformation(writeReq, STATUS_SUCCESS,
                                              bytesWritten);

            /* Try to satisfy pending reads on the peer side. */
            if (PeerReadQueue) {
                GcomDrainRingToReads(Ring, PeerReadQueue);
            }
        } else {
            /* Ring still full — re-queue. */
            WdfRequestForwardToIoQueue(writeReq, WriteQueue);
            break;
        }
    }
}


/* ── Check WaitCommEvent mask ─────────────────────────────────── */

VOID
GcomCheckWaitMask(
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG Events
)
{
    /*
     * If the COM side has a pending WaitCommEvent (IOCTL_SERIAL_WAIT_ON_MASK)
     * and the event bits match the wait mask, complete the request.
     */
    ULONG waitMask = PortPair->SignalState.WaitMask;
    ULONG matchedEvents = Events & waitMask;

    if (matchedEvents == 0 || !PortPair->WaitMaskQueue) {
        return;
    }

    WDFREQUEST waitReq;
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(
            PortPair->WaitMaskQueue, &waitReq)))
    {
        PULONG output;
        size_t outputLen;
        NTSTATUS st = WdfRequestRetrieveOutputBuffer(
            waitReq, sizeof(ULONG), (PVOID*)&output, &outputLen
        );
        if (NT_SUCCESS(st)) {
            *output = matchedEvents;
            WdfRequestCompleteWithInformation(waitReq, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(waitReq, st);
        }
    }
}
