/*
 * driver.h — Master header for the node-null virtual COM port driver.
 *
 * KMDF virtual serial port driver that creates COM port / companion
 * device pairs. External applications open the COM port; the node-null
 * native addon opens the companion device. Data and signals flow
 * between them through the driver.
 */

#pragma once

/* ── Windows / WDK headers ────────────────────────────────────── */

#include <ntddk.h>
#include <wdf.h>
#include <wdmsec.h>        /* SDDL_DEVOBJ_* security descriptors */
#include <ntddser.h>       /* Serial IOCTL definitions */
#include <ntstrsafe.h>     /* Safe string functions */

#include "ioctls.h"
#include "ringbuf.h"

/* ── Missing serial constants ────────────────────────────────── */
/* These are hardware register bit definitions not always in ntddser.h */

#ifndef SERIAL_MCR_DTR
#define SERIAL_MCR_DTR  0x01
#endif
#ifndef SERIAL_MCR_RTS
#define SERIAL_MCR_RTS  0x02
#endif

/* ── Pool tag ─────────────────────────────────────────────────── */

#define VCOM_POOL_TAG  'mocV'   /* 'Vcom' reversed for little-endian */

/* ── Limits ───────────────────────────────────────────────────── */

#define VCOM_MAX_PORTS              64
#define VCOM_RING_BUFFER_SIZE       (64 * 1024)  /* 64 KB per direction */

/* ── Version ──────────────────────────────────────────────────── */

#define VCOM_VERSION_MAJOR  0
#define VCOM_VERSION_MINOR  1
#define VCOM_VERSION_PATCH  0

/* ── Forward declarations ─────────────────────────────────────── */

typedef struct _VCOM_PORT_PAIR   VCOM_PORT_PAIR,   *PVCOM_PORT_PAIR;
typedef struct _VCOM_DEVICE_CTX  VCOM_DEVICE_CTX,  *PVCOM_DEVICE_CTX;

/* ── Port pair — one COM device + one companion device ────────── */

typedef struct _VCOM_PORT_PAIR {
    /* Identification */
    ULONG               PortNumber;       /* COM port number (e.g., 10) */
    ULONG               CompanionIndex;   /* Companion device index */
    BOOLEAN              Active;

    /* Device objects */
    WDFDEVICE            ComDevice;        /* The serial port device */
    WDFDEVICE            CompanionDevice;  /* The companion device */

    /* Symbolic link names */
    UNICODE_STRING       ComSymLink;       /* \\DosDevices\\COM<N> */
    UNICODE_STRING       CompSymLink;      /* \\DosDevices\\VCOMCompanion<N> */

    /* Open state */
    volatile LONG        ComSideOpen;
    volatile LONG        CompanionSideOpen;

    /* Ring buffers */
    VCOM_RING_BUFFER     ComToCompanion;   /* COM writes → companion reads */
    VCOM_RING_BUFFER     CompanionToCom;   /* Companion writes → COM reads */

    /* Pending I/O queues */
    WDFQUEUE             ComReadQueue;     /* Pending COM-side reads */
    WDFQUEUE             ComWriteQueue;    /* Pending COM-side writes (when ring full) */
    WDFQUEUE             CompReadQueue;    /* Pending companion reads */
    WDFQUEUE             CompWriteQueue;   /* Pending companion writes (when ring full) */

    /* Signal state (COM side configuration) */
    VCOM_SIGNAL_STATE    SignalState;
    WDFSPINLOCK          SignalLock;

    /* Companion-side output signals (null-modem crossover) */
    BOOLEAN              CompDtr;          /* Companion DTR → COM sees DSR+DCD */
    BOOLEAN              CompRts;          /* Companion RTS → COM sees CTS */

    /* Pending signal wait queue (companion side) */
    WDFQUEUE             SignalWaitQueue;

    /* COM-side WaitCommEvent queue */
    WDFQUEUE             WaitMaskQueue;

    /* Serial timeouts (set by COM side) */
    SERIAL_TIMEOUTS      Timeouts;

    /* Statistics */
    SERIALPERF_STATS     PerfStats;

    /* Reference count for safe teardown */
    volatile LONG        RefCount;

} VCOM_PORT_PAIR, *PVCOM_PORT_PAIR;


/* ── Driver device context (FDO) ──────────────────────────────── */

typedef struct _VCOM_DEVICE_CTX {
    /* Port pair table */
    PVCOM_PORT_PAIR      Ports[VCOM_MAX_PORTS];
    ULONG                PortCount;
    WDFSPINLOCK          PortTableLock;

    /* Control device */
    WDFDEVICE            ControlDevice;

    /* Next companion index (monotonically increasing) */
    volatile LONG        NextCompanionIndex;

} VCOM_DEVICE_CTX, *PVCOM_DEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VCOM_DEVICE_CTX, VcomGetDeviceContext)


/* ── Per-device context for COM / companion devices ──────────── */

typedef struct _VCOM_PORT_DEVICE_CTX {
    PVCOM_PORT_PAIR  PortPair;
    BOOLEAN          IsComSide;     /* TRUE = COM, FALSE = companion */
} VCOM_PORT_DEVICE_CTX, *PVCOM_PORT_DEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VCOM_PORT_DEVICE_CTX, VcomGetPortDeviceContext)


/* ── Per-file-object context (to identify COM vs companion) ──── */

typedef enum _VCOM_FILE_TYPE {
    VcomFileTypeControl,
    VcomFileTypeCom,
    VcomFileTypeCompanion,
} VCOM_FILE_TYPE;

typedef struct _VCOM_FILE_CTX {
    VCOM_FILE_TYPE  FileType;
    PVCOM_PORT_PAIR PortPair;       /* NULL for control device files */
} VCOM_FILE_CTX, *PVCOM_FILE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(VCOM_FILE_CTX, VcomGetFileContext)


/* ── Function prototypes: driver.c ────────────────────────────── */

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD  VcomEvtDeviceAdd;

/* ── Function prototypes: control.c ───────────────────────────── */

NTSTATUS VcomControlDeviceCreate(
    _In_ WDFDEVICE ParentDevice,
    _In_ PVCOM_DEVICE_CTX DevCtx
);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VcomControlIoDeviceControl;

/* ── Function prototypes: comport.c ───────────────────────────── */

/*
 * Create the COM port control device with I/O queues.
 *
 * This creates a WDFDEVICE named \Device\VCOMSerial<N> with a
 * symbolic link \DosDevices\COM<N>, default I/O queue dispatching
 * reads/writes/IOCTLs, and manual queues for pending I/O.
 */
NTSTATUS VcomComPortCreate(
    _In_ WDFDRIVER Driver,
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ PVCOM_PORT_PAIR PortPair,
    _In_ ULONG PortNumber
);

VOID VcomComPortDestroy(
    _In_ PVCOM_PORT_PAIR PortPair
);

EVT_WDF_IO_QUEUE_IO_READ           VcomComEvtRead;
EVT_WDF_IO_QUEUE_IO_WRITE          VcomComEvtWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VcomComEvtIoctl;
EVT_WDF_DEVICE_FILE_CREATE          VcomComEvtFileCreate;
EVT_WDF_FILE_CLOSE                  VcomComEvtFileClose;

/* ── Function prototypes: companion.c ─────────────────────────── */

/*
 * Create the companion control device with I/O queues.
 *
 * This creates a WDFDEVICE named \Device\VCOMCompanion<N> with a
 * symbolic link \DosDevices\VCOMCompanion<N>.
 */
NTSTATUS VcomCompanionCreate(
    _In_ WDFDRIVER Driver,
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ PVCOM_PORT_PAIR PortPair
);

VOID VcomCompanionDestroy(
    _In_ PVCOM_PORT_PAIR PortPair
);

EVT_WDF_IO_QUEUE_IO_READ           VcomCompEvtRead;
EVT_WDF_IO_QUEUE_IO_WRITE          VcomCompEvtWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL VcomCompEvtIoctl;
EVT_WDF_DEVICE_FILE_CREATE          VcomCompEvtFileCreate;
EVT_WDF_FILE_CLOSE                  VcomCompEvtFileClose;

/* ── Function prototypes: portpair.c ──────────────────────────── */

NTSTATUS VcomPortPairCreate(
    _In_  WDFDRIVER Driver,
    _In_  PVCOM_DEVICE_CTX DevCtx,
    _In_  ULONG RequestedPortNumber,
    _Out_ PVCOM_PORT_PAIR* OutPortPair
);

VOID VcomPortPairDestroy(
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ PVCOM_PORT_PAIR PortPair
);

/* Find an unused COM port number. */
ULONG VcomFindFreePortNumber(
    _In_ PVCOM_DEVICE_CTX DevCtx
);

/* Notify companion-side signal waiters of a change. */
VOID VcomSignalChanged(
    _In_ PVCOM_PORT_PAIR PortPair,
    _In_ ULONG ChangedBits
);

/* Satisfy pending reads from the ring buffer. */
VOID VcomDrainRingToReads(
    _In_ PVCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE ReadQueue
);

/* Satisfy pending writes into the ring buffer. */
VOID VcomDrainWritesToRing(
    _In_ PVCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE WriteQueue,
    _In_ WDFQUEUE PeerReadQueue
);

/* Complete pending WaitCommEvent IRPs if appropriate. */
VOID VcomCheckWaitMask(
    _In_ PVCOM_PORT_PAIR PortPair,
    _In_ ULONG Events
);
