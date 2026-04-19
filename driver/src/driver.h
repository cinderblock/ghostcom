/*
 * driver.h — Master header for the GhostCOM virtual COM port driver.
 *
 * KMDF virtual serial port driver that creates COM port / companion
 * device pairs. External applications open the COM port; the GhostCOM
 * native addon opens the companion device. Data and signals flow
 * between them through the driver.
 */

#pragma once

/* ── Windows / WDK headers ────────────────────────────────────── */

/* Ensure KMDF version is defined before including WDF headers. */
#ifndef KMDF_VERSION_MAJOR
#define KMDF_VERSION_MAJOR 1
#endif
#ifndef KMDF_VERSION_MINOR
#define KMDF_VERSION_MINOR 33
#endif

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

#define GCOM_POOL_TAG  'mocG'   /* 'Gcom' reversed for little-endian */

/* ── Limits ───────────────────────────────────────────────────── */

#define GCOM_MAX_PORTS              64
#define GCOM_RING_BUFFER_SIZE       (64 * 1024)  /* 64 KB per direction */

/* ── Version ──────────────────────────────────────────────────── */

#define GCOM_VERSION_MAJOR  0
#define GCOM_VERSION_MINOR  1
#define GCOM_VERSION_PATCH  0

/* ── Forward declarations ─────────────────────────────────────── */

typedef struct _GCOM_PORT_PAIR   GCOM_PORT_PAIR,   *PGCOM_PORT_PAIR;
typedef struct _GCOM_DEVICE_CTX  GCOM_DEVICE_CTX,  *PGCOM_DEVICE_CTX;

/*
 * Identification description passed via the FDO's default child list.
 * Size used in both WDF_CHILD_LIST_CONFIG_INIT (driver.c) and
 * WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER_INIT (comport.c) MUST
 * agree — WDF memcpys this many bytes when the child is reported.
 */
typedef struct _GCOM_CHILD_ID {
    WDF_CHILD_IDENTIFICATION_DESCRIPTION_HEADER Header;
    ULONG PortNumber;
} GCOM_CHILD_ID, *PGCOM_CHILD_ID;

/*
 * Sentinel value stored in Ports[] to reserve a slot during port pair
 * creation.  All table scans must use GCOM_PORT_IS_VALID() before
 * dereferencing a Ports[] entry.
 */
#define GCOM_PORT_RESERVED  ((PGCOM_PORT_PAIR)(ULONG_PTR)1)
#define GCOM_PORT_IS_VALID(p) ((p) != NULL && (p) != GCOM_PORT_RESERVED)

/* ── Port pair — one COM device + one companion device ────────── */

typedef struct _GCOM_PORT_PAIR {
    /* Identification */
    ULONG               PortNumber;       /* COM port number (e.g., 10) */
    ULONG               CompanionIndex;   /* Companion device index */
    volatile LONG        Active;

    /* Device objects */
    WDFDEVICE            ComDevice;        /* The serial port device (control device) */
    WDFDEVICE            CompanionDevice;  /* The companion device */
    WDFDEVICE            PnpPdo;           /* Shadow PDO for Device Manager visibility */

    /* Symbolic link names */
    UNICODE_STRING       ComSymLink;       /* \\DosDevices\\COM<N> */
    UNICODE_STRING       CompSymLink;      /* \\DosDevices\\GCOM<N> */

    /* Open state */
    volatile LONG        ComSideOpen;
    volatile LONG        CompanionSideOpen;

    /* Ring buffers */
    GCOM_RING_BUFFER     ComToCompanion;   /* COM writes → companion reads */
    GCOM_RING_BUFFER     CompanionToCom;   /* Companion writes → COM reads */

    /* Pending I/O queues */
    WDFQUEUE             ComReadQueue;     /* Pending COM-side reads */
    WDFQUEUE             ComWriteQueue;    /* Pending COM-side writes (when ring full) */
    WDFQUEUE             CompReadQueue;    /* Pending companion reads */
    WDFQUEUE             CompWriteQueue;   /* Pending companion writes (when ring full) */

    /* Signal state (COM side configuration) */
    GCOM_SIGNAL_STATE    SignalState;
    WDFSPINLOCK          SignalLock;

    /* Data lock — serializes all ring buffer operations across both sides. */
    WDFSPINLOCK          DataLock;

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

} GCOM_PORT_PAIR, *PGCOM_PORT_PAIR;


/* ── Driver device context (FDO) ──────────────────────────────── */

typedef struct _GCOM_DEVICE_CTX {
    /* Parent FDO — needed for WdfPdoInitAllocate (shadow PDOs). */
    WDFDEVICE            FdoDevice;

    /* Port pair table */
    PGCOM_PORT_PAIR      Ports[GCOM_MAX_PORTS];
    ULONG                PortCount;
    WDFWAITLOCK           PortTableLock;

    /* Control device */
    WDFDEVICE            ControlDevice;

    /* Next companion index (monotonically increasing) */
    volatile LONG        NextCompanionIndex;

} GCOM_DEVICE_CTX, *PGCOM_DEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCOM_DEVICE_CTX, GcomGetDeviceContext)


/* ── Per-device context for COM / companion devices ──────────── */

typedef struct _GCOM_PORT_DEVICE_CTX {
    PGCOM_PORT_PAIR  PortPair;
    BOOLEAN          IsComSide;     /* TRUE = COM, FALSE = companion */
} GCOM_PORT_DEVICE_CTX, *PGCOM_PORT_DEVICE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCOM_PORT_DEVICE_CTX, GcomGetPortDeviceContext)


/* ── Per-file-object context (to identify COM vs companion) ──── */

typedef enum _GCOM_FILE_TYPE {
    GcomFileTypeControl,
    GcomFileTypeCom,
    GcomFileTypeCompanion,
} GCOM_FILE_TYPE;

typedef struct _GCOM_FILE_CTX {
    GCOM_FILE_TYPE  FileType;
    PGCOM_PORT_PAIR PortPair;       /* NULL for control device files */
} GCOM_FILE_CTX, *PGCOM_FILE_CTX;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(GCOM_FILE_CTX, GcomGetFileContext)


/* ── Function prototypes: driver.c ────────────────────────────── */

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD             GcomEvtDeviceAdd;
EVT_WDF_DEVICE_D0_ENTRY               GcomEvtDeviceD0Entry;
EVT_WDF_DEVICE_D0_EXIT                GcomEvtDeviceD0Exit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP GcomEvtSelfManagedIoCleanup;

/* ── Function prototypes: control.c ───────────────────────────── */

NTSTATUS GcomControlDeviceCreate(
    _In_ WDFDEVICE ParentDevice,
    _In_ PGCOM_DEVICE_CTX DevCtx
);

VOID GcomControlDeviceInvalidate(VOID);

EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GcomControlIoDeviceControl;

/* ── Function prototypes: comport.c ───────────────────────────── */

/*
 * Create the COM port control device with I/O queues.
 *
 * This creates a WDFDEVICE named \Device\GCOMSerial<N> with a
 * symbolic link \DosDevices\COM<N>, default I/O queue dispatching
 * reads/writes/IOCTLs, and manual queues for pending I/O.
 */
NTSTATUS GcomComPortCreate(
    _In_ WDFDRIVER Driver,
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG PortNumber
);

VOID GcomComPortDestroy(
    _In_ PGCOM_PORT_PAIR PortPair
);

EVT_WDF_IO_QUEUE_IO_READ           GcomComEvtRead;
EVT_WDF_IO_QUEUE_IO_WRITE          GcomComEvtWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GcomComEvtIoctl;
EVT_WDF_DEVICE_FILE_CREATE          GcomComEvtFileCreate;
EVT_WDF_FILE_CLOSE                  GcomComEvtFileClose;

/* ── Function prototypes: companion.c ─────────────────────────── */

/*
 * Create the companion control device with I/O queues.
 *
 * This creates a WDFDEVICE named \Device\GCOM<N> with a
 * symbolic link \DosDevices\GCOM<N>.
 */
NTSTATUS GcomCompanionCreate(
    _In_ WDFDRIVER Driver,
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ PGCOM_PORT_PAIR PortPair
);

VOID GcomCompanionDestroy(
    _In_ PGCOM_PORT_PAIR PortPair
);

EVT_WDF_IO_QUEUE_IO_READ           GcomCompEvtRead;
EVT_WDF_IO_QUEUE_IO_WRITE          GcomCompEvtWrite;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL GcomCompEvtIoctl;
EVT_WDF_DEVICE_FILE_CREATE          GcomCompEvtFileCreate;
EVT_WDF_FILE_CLOSE                  GcomCompEvtFileClose;

/* ── Function prototypes: portpair.c ──────────────────────────── */

NTSTATUS GcomPortPairCreate(
    _In_  WDFDRIVER Driver,
    _In_  PGCOM_DEVICE_CTX DevCtx,
    _In_  ULONG RequestedPortNumber,
    _Out_ PGCOM_PORT_PAIR* OutPortPair
);

VOID GcomPortPairDestroy(
    _In_ PGCOM_DEVICE_CTX DevCtx,
    _In_ PGCOM_PORT_PAIR PortPair
);

/* Reference counting for safe teardown. */
VOID GcomPortPairAddRef(
    _In_ PGCOM_PORT_PAIR PortPair
);

VOID GcomPortPairRelease(
    _In_ PGCOM_PORT_PAIR PortPair
);

/* Find an unused COM port number. */
ULONG GcomFindFreePortNumber(
    _In_ PGCOM_DEVICE_CTX DevCtx
);

/* Notify companion-side signal waiters of a change. */
VOID GcomSignalChanged(
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG ChangedBits
);

/* Satisfy pending reads from the ring buffer. */
VOID GcomDrainRingToReads(
    _In_ PGCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE ReadQueue
);

/* Satisfy pending writes into the ring buffer. */
VOID GcomDrainWritesToRing(
    _In_ PGCOM_RING_BUFFER Ring,
    _In_ WDFQUEUE WriteQueue,
    _In_ WDFQUEUE PeerReadQueue
);

/* Complete pending WaitCommEvent IRPs if appropriate. */
VOID GcomCheckWaitMask(
    _In_ PGCOM_PORT_PAIR PortPair,
    _In_ ULONG Events
);
