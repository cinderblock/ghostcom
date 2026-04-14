/*
 * ioctls.h — IOCTL codes and shared data structures.
 *
 * This header defines the protocol between the kernel driver and
 * user-mode code. Both the driver and the Rust addon must agree on
 * these definitions exactly.
 *
 * IMPORTANT: Any change here must be mirrored in addon/src/ioctl.rs.
 */

#pragma once

#include <ntddser.h>  /* For SERIAL_* constants */

/* ── Device type ──────────────────────────────────────────────── */

#define GCOM_DEVICE_TYPE  0x8001

/* ── Control device IOCTLs ────────────────────────────────────── */

/* Create a new virtual COM port pair (COM device + companion). */
#define IOCTL_GCOM_CREATE_PORT \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Destroy an existing virtual COM port pair. */
#define IOCTL_GCOM_DESTROY_PORT \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* List all active virtual COM port pairs. */
#define IOCTL_GCOM_LIST_PORTS \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Query driver version. */
#define IOCTL_GCOM_GET_VERSION \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS)

/* ── Companion device IOCTLs ──────────────────────────────────── */

/*
 * Wait for a signal change on the COM side (overlapped).
 *
 * The driver holds this IRP until any serial configuration or
 * control signal changes on the COM side, then completes it with
 * a GCOM_SIGNAL_STATE payload.
 *
 * The user-mode caller should immediately re-issue this IOCTL
 * after processing the result.
 */
#define IOCTL_GCOM_WAIT_SIGNAL_CHANGE \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x810, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Get current signal state snapshot (synchronous). */
#define IOCTL_GCOM_GET_SIGNALS \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x811, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Set companion-side output signals (DTR, RTS). */
#define IOCTL_GCOM_SET_SIGNALS \
    CTL_CODE(GCOM_DEVICE_TYPE, 0x812, METHOD_BUFFERED, FILE_WRITE_ACCESS)


/* ── Data structures ──────────────────────────────────────────── */

#pragma pack(push, 1)

/* Request for IOCTL_GCOM_CREATE_PORT. */
typedef struct _GCOM_CREATE_PORT_REQUEST {
    ULONG PortNumber;         /* 0 = auto-assign */
} GCOM_CREATE_PORT_REQUEST, *PGCOM_CREATE_PORT_REQUEST;

/* Response for IOCTL_GCOM_CREATE_PORT. */
typedef struct _GCOM_CREATE_PORT_RESPONSE {
    ULONG PortNumber;         /* Assigned COM port number */
    ULONG CompanionIndex;     /* Companion device index */
} GCOM_CREATE_PORT_RESPONSE, *PGCOM_CREATE_PORT_RESPONSE;

/* Request for IOCTL_GCOM_DESTROY_PORT. */
typedef struct _GCOM_DESTROY_PORT_REQUEST {
    ULONG CompanionIndex;
} GCOM_DESTROY_PORT_REQUEST, *PGCOM_DESTROY_PORT_REQUEST;

/* Per-port info in list response. */
typedef struct _GCOM_PORT_INFO {
    ULONG PortNumber;
    ULONG CompanionIndex;
    ULONG ComSideOpen;        /* BOOLEAN as ULONG */
    ULONG CompanionSideOpen;  /* BOOLEAN as ULONG */
} GCOM_PORT_INFO, *PGCOM_PORT_INFO;

/* Header for IOCTL_GCOM_LIST_PORTS response. */
typedef struct _GCOM_LIST_PORTS_HEADER {
    ULONG Count;
} GCOM_LIST_PORTS_HEADER, *PGCOM_LIST_PORTS_HEADER;

/* Driver version info. */
typedef struct _GCOM_VERSION_INFO {
    USHORT Major;
    USHORT Minor;
    USHORT Patch;
    USHORT Reserved;
} GCOM_VERSION_INFO, *PGCOM_VERSION_INFO;

/*
 * Complete signal state snapshot.
 *
 * Returned by IOCTL_GCOM_GET_SIGNALS and IOCTL_GCOM_WAIT_SIGNAL_CHANGE.
 */
typedef struct _GCOM_SIGNAL_STATE {
    ULONG SequenceNumber;     /* Monotonic counter */
    ULONG ChangedMask;        /* Bitmask of changes */

    /* Serial line configuration */
    ULONG BaudRate;
    UCHAR StopBits;           /* STOP_BIT_1, STOP_BIT_1_5, STOP_BIT_2 */
    UCHAR Parity;             /* NO_PARITY, ODD_PARITY, etc. */
    UCHAR DataBits;           /* 5, 6, 7, 8 */
    UCHAR _Pad0;

    /* Modem control lines (set by COM side) */
    ULONG DtrState;           /* BOOLEAN as ULONG */
    ULONG RtsState;           /* BOOLEAN as ULONG */
    ULONG BreakState;         /* BOOLEAN as ULONG */

    /* Flow control: SERIAL_HANDFLOW */
    ULONG ControlHandShake;
    ULONG FlowReplace;
    LONG  XonLimit;
    LONG  XoffLimit;

    /* Special chars: SERIAL_CHARS */
    UCHAR EofChar;
    UCHAR ErrorChar;
    UCHAR BreakChar;
    UCHAR EventChar;
    UCHAR XonChar;
    UCHAR XoffChar;
    UCHAR _Pad1[2];

    /* COM-side WaitCommEvent mask */
    ULONG WaitMask;
} GCOM_SIGNAL_STATE, *PGCOM_SIGNAL_STATE;

/* Payload for IOCTL_GCOM_SET_SIGNALS. */
typedef struct _GCOM_SET_SIGNALS {
    ULONG DtrState;           /* BOOLEAN as ULONG */
    ULONG RtsState;           /* BOOLEAN as ULONG */
} GCOM_SET_SIGNALS, *PGCOM_SET_SIGNALS;

#pragma pack(pop)


/* ── ChangedMask bit definitions ──────────────────────────────── */

#define GCOM_CHANGED_BAUD       0x0001
#define GCOM_CHANGED_LINE_CTRL  0x0002
#define GCOM_CHANGED_DTR        0x0004
#define GCOM_CHANGED_RTS        0x0008
#define GCOM_CHANGED_BREAK      0x0010
#define GCOM_CHANGED_HANDFLOW   0x0020
#define GCOM_CHANGED_CHARS      0x0040
#define GCOM_CHANGED_WAIT_MASK  0x0080
#define GCOM_CHANGED_COM_OPEN   0x0100
#define GCOM_CHANGED_COM_CLOSE  0x0200
