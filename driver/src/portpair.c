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
            if (GCOM_PORT_IS_VALID(DevCtx->Ports[i]) &&
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
            continue;  /* COM symlink taken (stale or active). */
        }

        /* Also check the NT device name and companion symlink —
         * these can be stale after a crash even if the DOS symlink
         * was cleaned up. */
        WCHAR devName[64], compName[64];
        RtlStringCbPrintfW(devName, sizeof(devName),
                           L"\\Device\\GCOMSerial%lu", candidate);
        RtlStringCbPrintfW(compName, sizeof(compName),
                           L"\\DosDevices\\GCOM%lu", candidate);

        UNICODE_STRING devStr, compStr;
        RtlInitUnicodeString(&devStr, devName);
        RtlInitUnicodeString(&compStr, compName);

        OBJECT_ATTRIBUTES devAttr, compAttr;
        InitializeObjectAttributes(&devAttr, &devStr,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        InitializeObjectAttributes(&compAttr, &compStr,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);

        /* Check for stale device object. */
        HANDLE devHandle;
        st = ZwOpenSymbolicLinkObject(&devHandle, SYMBOLIC_LINK_QUERY, &compAttr);
        if (NT_SUCCESS(st)) {
            ZwClose(devHandle);
            continue;  /* Companion symlink stale. */
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
    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);

    slotIndex = GCOM_MAX_PORTS;
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (DevCtx->Ports[i] == NULL) {
            slotIndex = i;
            break;
        }
    }

    if (slotIndex == GCOM_MAX_PORTS) {
        WdfWaitLockRelease(DevCtx->PortTableLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    /* Determine port number. */
    ULONG portNumber;
    if (RequestedPortNumber != 0) {
        /* Check if the requested number is already in use by us. */
        for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
            if (GCOM_PORT_IS_VALID(DevCtx->Ports[i]) &&
                DevCtx->Ports[i]->Active &&
                DevCtx->Ports[i]->PortNumber == RequestedPortNumber)
            {
                WdfWaitLockRelease(DevCtx->PortTableLock);
                return STATUS_OBJECT_NAME_COLLISION;
            }
        }

        /*
         * Also check whether the \DosDevices\COM<N> symlink already exists
         * in the object namespace (e.g. from a crashed previous session that
         * didn't clean up).  GcomFindFreePortNumber does this check for
         * auto-assigned ports but the explicit-request path didn't, leading
         * to STATUS_OBJECT_NAME_COLLISION from WdfDeviceCreateSymbolicLink
         * later with no clear error for the caller.
         */
        WCHAR dosName[64];
        UNICODE_STRING dosNameStr;
        RtlStringCbPrintfW(dosName, sizeof(dosName),
                           L"\\DosDevices\\COM%lu", RequestedPortNumber);
        RtlInitUnicodeString(&dosNameStr, dosName);

        OBJECT_ATTRIBUTES objAttr;
        InitializeObjectAttributes(&objAttr, &dosNameStr,
                                   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                                   NULL, NULL);
        HANDLE linkHandle;
        NTSTATUS st = ZwOpenSymbolicLinkObject(&linkHandle,
                                               SYMBOLIC_LINK_QUERY, &objAttr);
        if (NT_SUCCESS(st)) {
            ZwClose(linkHandle);
            WdfWaitLockRelease(DevCtx->PortTableLock);
            /* Stale symlink from a crashed session — caller should retry. */
            return STATUS_OBJECT_NAME_COLLISION;
        }

        portNumber = RequestedPortNumber;
    } else {
        portNumber = GcomFindFreePortNumber(DevCtx);
        if (portNumber == 0) {
            WdfWaitLockRelease(DevCtx->PortTableLock);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    /*
     * Use portNumber as the companion index so that COM<N> is always
     * paired with GCOM<N>. The auto-incrementing NextCompanionIndex
     * caused GCOM0 to be created for COM10, making the companion device
     * name confusing and inconsistent with the port number.
     */
    ULONG companionIndex = portNumber;

    /*
     * Reserve the slot with a sentinel before releasing the lock.
     * This prevents another concurrent create from claiming the
     * same slot (TOCTOU fix).
     */
    DevCtx->Ports[slotIndex] = GCOM_PORT_RESERVED;
    DevCtx->PortCount++;

    WdfWaitLockRelease(DevCtx->PortTableLock);

    /* Allocate the port pair structure. */
    pp = (PGCOM_PORT_PAIR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(GCOM_PORT_PAIR),
        GCOM_POOL_TAG
    );
    if (!pp) {
        WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
        DevCtx->Ports[slotIndex] = NULL;
        DevCtx->PortCount--;
        WdfWaitLockRelease(DevCtx->PortTableLock);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pp, sizeof(GCOM_PORT_PAIR));
    pp->PortNumber = portNumber;
    pp->CompanionIndex = companionIndex;
    InterlockedExchange(&pp->Active, TRUE);
    pp->RefCount = 1;

    /* Initialize ring buffers. */
    status = GcomRingInit(&pp->ComToCompanion, GCOM_RING_BUFFER_SIZE);
    if (!NT_SUCCESS(status)) {
        goto fail_alloc;
    }

    status = GcomRingInit(&pp->CompanionToCom, GCOM_RING_BUFFER_SIZE);
    if (!NT_SUCCESS(status)) {
        goto fail_alloc;
    }

    /* Initialize locks. */
    WDF_OBJECT_ATTRIBUTES lockAttr;
    WDF_OBJECT_ATTRIBUTES_INIT(&lockAttr);

    status = WdfSpinLockCreate(&lockAttr, &pp->SignalLock);
    if (!NT_SUCCESS(status)) {
        goto fail_alloc;
    }

    status = WdfSpinLockCreate(&lockAttr, &pp->DataLock);
    if (!NT_SUCCESS(status)) {
        goto fail_alloc;
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

    /* Replace the sentinel with the real pointer BEFORE creating
     * devices, so that device creation callbacks can find the entry. */
    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
    DevCtx->Ports[slotIndex] = pp;
    WdfWaitLockRelease(DevCtx->PortTableLock);

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
    /* Remove from table (entry holds the real pp pointer). */
    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
    DevCtx->Ports[slotIndex] = NULL;
    DevCtx->PortCount--;
    WdfWaitLockRelease(DevCtx->PortTableLock);

fail_alloc:
    /* Clean up partially-initialized port pair.
     * All fields are zero-initialized, so NULL/0 checks are safe. */
    if (pp->DataLock) {
        WdfObjectDelete(pp->DataLock);
    }
    if (pp->SignalLock) {
        WdfObjectDelete(pp->SignalLock);
    }
    GcomRingFree(&pp->CompanionToCom);
    GcomRingFree(&pp->ComToCompanion);

    /* Release the sentinel slot if we haven't stored the real pointer yet. */
    if (DevCtx->Ports[slotIndex] == GCOM_PORT_RESERVED) {
        WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
        DevCtx->Ports[slotIndex] = NULL;
        DevCtx->PortCount--;
        WdfWaitLockRelease(DevCtx->PortTableLock);
    }

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
    TraceEvents(0, 0, "Port pair destroy starting: COM%lu", PortPair->PortNumber);

    /* Mark inactive so I/O callbacks bail out early. */
    InterlockedExchange(&PortPair->Active, FALSE);

    /* Remove from the port table (idempotent — may already be removed). */
    WdfWaitLockAcquire(DevCtx->PortTableLock, NULL);
    for (ULONG i = 0; i < GCOM_MAX_PORTS; i++) {
        if (DevCtx->Ports[i] == PortPair) {
            DevCtx->Ports[i] = NULL;
            DevCtx->PortCount--;
            break;
        }
    }
    WdfWaitLockRelease(DevCtx->PortTableLock);

    /*
     * NULL out queue handles under SignalLock BEFORE deleting devices.
     * This prevents GcomSignalChanged / GcomCheckWaitMask from using
     * stale queue handles if they're running concurrently on another CPU.
     */
    WdfSpinLockAcquire(PortPair->SignalLock);
    PortPair->SignalWaitQueue = NULL;
    PortPair->WaitMaskQueue = NULL;
    WdfSpinLockRelease(PortPair->SignalLock);

    PortPair->ComReadQueue = NULL;
    PortPair->ComWriteQueue = NULL;
    PortPair->CompReadQueue = NULL;
    PortPair->CompWriteQueue = NULL;

    /*
     * Delete the COM port and companion WDFDEVICE objects.
     *
     * WdfObjectDelete on a device automatically:
     * - Cancels and completes all pending I/O on all queues
     * - Deletes all child objects (queues, timers, etc.)
     * - Removes symbolic links created with WdfDeviceCreateSymbolicLink
     *
     * We delete devices BEFORE freeing ring buffers so that all
     * in-flight I/O callbacks (on parallel-dispatch queues) are
     * drained before the ring buffer memory is freed.
     *
     * Do NOT call WdfIoQueuePurgeSynchronously separately;
     * that can race with WdfObjectDelete and cause WDF_VIOLATION.
     */
    TraceEvents(0, 0, "Deleting COM device for COM%lu", PortPair->PortNumber);
    GcomComPortDestroy(PortPair);

    TraceEvents(0, 0, "Deleting companion device for COM%lu", PortPair->PortNumber);
    GcomCompanionDestroy(PortPair);

    /* Now safe to free ring buffers — no I/O callbacks can be running. */
    GcomRingFree(&PortPair->ComToCompanion);
    GcomRingFree(&PortPair->CompanionToCom);

    /* Delete WDF lock objects (created without a parent device). */
    if (PortPair->DataLock) {
        WdfObjectDelete(PortPair->DataLock);
        PortPair->DataLock = NULL;
    }
    if (PortPair->SignalLock) {
        WdfObjectDelete(PortPair->SignalLock);
        PortPair->SignalLock = NULL;
    }

    TraceEvents(0, 0, "Port pair destroyed: COM%lu — releasing creation ref",
                PortPair->PortNumber);

    /* Drop the creation reference. The struct is freed when RefCount hits 0. */
    GcomPortPairRelease(PortPair);
}


/* ── Signal change notification ───────────────────────────────── */

VOID
GcomSignalChanged(
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG ChangedBits
)
{
    if (!PortPair->Active) {
        return;
    }

    /*
     * Increment the sequence number and set the changed bits.
     * Snapshot the queue handle under lock so the destroy path
     * can safely NULL it before deleting the device.
     */
    WdfSpinLockAcquire(PortPair->SignalLock);

    PortPair->SignalState.SequenceNumber++;
    PortPair->SignalState.ChangedMask |= ChangedBits;

    GCOM_SIGNAL_STATE snapshot = PortPair->SignalState;
    WDFQUEUE signalQueue = PortPair->SignalWaitQueue;

    WdfSpinLockRelease(PortPair->SignalLock);

    /* Complete all pending signal wait IRPs. */
    if (signalQueue) {
        WDFREQUEST waitReq;
        while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(
                signalQueue, &waitReq)))
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

    /* Clear only the bits that were actually delivered to waiters.
     * Any new bits set between the snapshot and now are preserved. */
    WdfSpinLockAcquire(PortPair->SignalLock);
    PortPair->SignalState.ChangedMask &= ~snapshot.ChangedMask;
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

    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_ERROR_LEVEL,
        "ghostcom [DRAIN]: ring empty=%d, attempting queue retrieve\n",
        GcomRingIsEmpty(Ring)));

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
            NTSTATUS fwdSt = WdfRequestForwardToIoQueue(readReq, ReadQueue);
            if (!NT_SUCCESS(fwdSt)) {
                TraceEvents(0, 0, "DrainRingToReads: forward failed 0x%08X", fwdSt);
                WdfRequestComplete(readReq, fwdSt);
            }
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
            NTSTATUS fwdSt = WdfRequestForwardToIoQueue(writeReq, WriteQueue);
            if (!NT_SUCCESS(fwdSt)) {
                TraceEvents(0, 0, "DrainWritesToRing: forward failed 0x%08X", fwdSt);
                WdfRequestComplete(writeReq, fwdSt);
            }
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
    if (!PortPair->Active) {
        return;
    }

    /*
     * If the COM side has a pending WaitCommEvent (IOCTL_SERIAL_WAIT_ON_MASK)
     * and the event bits match the wait mask, complete the request.
     *
     * Snapshot the queue handle under SignalLock so the destroy path
     * can safely NULL it before deleting the device.
     */
    WdfSpinLockAcquire(PortPair->SignalLock);
    ULONG waitMask = PortPair->SignalState.WaitMask;
    WDFQUEUE waitQueue = PortPair->WaitMaskQueue;
    WdfSpinLockRelease(PortPair->SignalLock);

    ULONG matchedEvents = Events & waitMask;

    if (matchedEvents == 0 || !waitQueue) {
        return;
    }

    WDFREQUEST waitReq;
    while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(
            waitQueue, &waitReq)))
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


/* ── Reference counting ──────────────────────────────────────── */

VOID
GcomPortPairAddRef(
    _In_ PGCOM_PORT_PAIR PortPair
)
{
    LONG newCount = InterlockedIncrement(&PortPair->RefCount);
    TraceEvents(0, 0, "COM%lu: AddRef → %ld", PortPair->PortNumber, newCount);
}

VOID
GcomPortPairRelease(
    _In_ PGCOM_PORT_PAIR PortPair
)
{
    LONG newCount = InterlockedDecrement(&PortPair->RefCount);
    TraceEvents(0, 0, "COM%lu: Release → %ld", PortPair->PortNumber, newCount);

    if (newCount == 0) {
        TraceEvents(0, 0, "COM%lu: RefCount hit 0, freeing port pair",
                    PortPair->PortNumber);
        ExFreePoolWithTag(PortPair, GCOM_POOL_TAG);
    }
}
