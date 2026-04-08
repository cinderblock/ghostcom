/*
 * ringbuf.h — Lock-free single-producer/single-consumer ring buffer.
 *
 * Used for data transfer between the COM side and the companion side.
 * Each port pair has two ring buffers (one per direction).
 *
 * The ring buffer uses power-of-two sizing for efficient modulo
 * operations (mask instead of division).
 *
 * Thread safety model:
 *   - WritePos is only modified by the producer.
 *   - ReadPos is only modified by the consumer.
 *   - Both are read by the other side to determine available space/data.
 *   - InterlockedExchange provides acquire/release semantics.
 *   - No spinlock is needed for the ring buffer itself.
 *   - The WDFSPINLOCK on the port pair is only for coordinating
 *     IRP queue operations with ring buffer state.
 */

#pragma once

#include <ntddk.h>

typedef struct _VCOM_RING_BUFFER {
    PUCHAR          Buffer;
    ULONG           Size;           /* Must be power of 2 */
    ULONG           Mask;           /* Size - 1 */
    volatile LONG   WritePos;       /* Next write position */
    volatile LONG   ReadPos;        /* Next read position */
} VCOM_RING_BUFFER, *PVCOM_RING_BUFFER;


/* ── Lifecycle ────────────────────────────────────────────────── */

/*
 * Allocate and initialize a ring buffer.
 *
 * Size must be a power of 2. Returns STATUS_SUCCESS or
 * STATUS_INSUFFICIENT_RESOURCES.
 */
static inline NTSTATUS
VcomRingInit(
    _Out_ PVCOM_RING_BUFFER Ring,
    _In_  ULONG Size
)
{
    /* Verify power of 2 */
    NT_ASSERT((Size & (Size - 1)) == 0);

    Ring->Buffer = (PUCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        Size,
        'mocV'
    );
    if (!Ring->Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    Ring->Size = Size;
    Ring->Mask = Size - 1;
    Ring->WritePos = 0;
    Ring->ReadPos = 0;

    return STATUS_SUCCESS;
}

/*
 * Free the ring buffer's memory.
 */
static inline VOID
VcomRingFree(
    _Inout_ PVCOM_RING_BUFFER Ring
)
{
    if (Ring->Buffer) {
        ExFreePoolWithTag(Ring->Buffer, 'mocV');
        Ring->Buffer = NULL;
    }
    Ring->Size = 0;
    Ring->Mask = 0;
    Ring->WritePos = 0;
    Ring->ReadPos = 0;
}

/* ── Query ────────────────────────────────────────────────────── */

/*
 * Number of bytes available to read.
 */
static inline ULONG
VcomRingReadAvailable(
    _In_ PVCOM_RING_BUFFER Ring
)
{
    LONG wp = InterlockedCompareExchange(&Ring->WritePos, 0, 0);
    LONG rp = InterlockedCompareExchange(&Ring->ReadPos, 0, 0);
    return (ULONG)((wp - rp) & (LONG)Ring->Mask);
}

/*
 * Number of bytes of free space for writing.
 */
static inline ULONG
VcomRingWriteAvailable(
    _In_ PVCOM_RING_BUFFER Ring
)
{
    /* Leave one byte unused to distinguish full from empty. */
    return Ring->Size - 1 - VcomRingReadAvailable(Ring);
}

/*
 * Is the ring buffer empty?
 */
static inline BOOLEAN
VcomRingIsEmpty(
    _In_ PVCOM_RING_BUFFER Ring
)
{
    return VcomRingReadAvailable(Ring) == 0;
}

/* ── Operations ───────────────────────────────────────────────── */

/*
 * Write bytes into the ring buffer (producer side).
 *
 * Returns the number of bytes actually written (may be less than
 * Length if the buffer is full).
 */
static inline ULONG
VcomRingWrite(
    _In_ PVCOM_RING_BUFFER Ring,
    _In_reads_bytes_(Length) const UCHAR* Data,
    _In_ ULONG Length
)
{
    ULONG available = VcomRingWriteAvailable(Ring);
    ULONG toWrite = min(Length, available);
    ULONG wp = (ULONG)Ring->WritePos & Ring->Mask;

    if (toWrite == 0) {
        return 0;
    }

    /* Handle wrap-around */
    ULONG firstChunk = min(toWrite, Ring->Size - wp);
    RtlCopyMemory(Ring->Buffer + wp, Data, firstChunk);

    if (toWrite > firstChunk) {
        RtlCopyMemory(Ring->Buffer, Data + firstChunk, toWrite - firstChunk);
    }

    /* Publish the new write position with release semantics. */
    InterlockedExchange(&Ring->WritePos,
                        (Ring->WritePos + (LONG)toWrite) & (LONG)Ring->Mask);

    return toWrite;
}

/*
 * Read bytes from the ring buffer (consumer side).
 *
 * Returns the number of bytes actually read (may be less than
 * Length if less data is available).
 */
static inline ULONG
VcomRingRead(
    _In_  PVCOM_RING_BUFFER Ring,
    _Out_writes_bytes_(Length) UCHAR* Data,
    _In_  ULONG Length
)
{
    ULONG available = VcomRingReadAvailable(Ring);
    ULONG toRead = min(Length, available);
    ULONG rp = (ULONG)Ring->ReadPos & Ring->Mask;

    if (toRead == 0) {
        return 0;
    }

    /* Handle wrap-around */
    ULONG firstChunk = min(toRead, Ring->Size - rp);
    RtlCopyMemory(Data, Ring->Buffer + rp, firstChunk);

    if (toRead > firstChunk) {
        RtlCopyMemory(Data + firstChunk, Ring->Buffer, toRead - firstChunk);
    }

    /* Publish the new read position with release semantics. */
    InterlockedExchange(&Ring->ReadPos,
                        (Ring->ReadPos + (LONG)toRead) & (LONG)Ring->Mask);

    return toRead;
}

/*
 * Discard all data in the ring buffer (used for PURGE).
 */
static inline VOID
VcomRingFlush(
    _In_ PVCOM_RING_BUFFER Ring
)
{
    LONG wp = InterlockedCompareExchange(&Ring->WritePos, 0, 0);
    InterlockedExchange(&Ring->ReadPos, wp);
}
