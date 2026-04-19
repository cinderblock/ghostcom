"""
Test ReadFileEx with APC completion on our virtual COM port.
This directly tests whether the driver correctly completes ReadFileEx IRPs
and queues APCs to the requesting thread.

Usage: python test-readfileex.py COM10
"""
import ctypes, ctypes.wintypes as wt, sys, time, threading

PORT = sys.argv[1] if len(sys.argv) > 1 else "COM10"
k = ctypes.WinDLL('kernel32', use_last_error=True)

# Types
LPOVERLAPPED_COMPLETION_ROUTINE = ctypes.WINFUNCTYPE(None, wt.DWORD, wt.DWORD, ctypes.c_void_p)

class OVERLAPPED(ctypes.Structure):
    _fields_ = [
        ("Internal",    ctypes.c_size_t),
        ("InternalHigh",ctypes.c_size_t),
        ("Offset",      wt.DWORD),
        ("OffsetHigh",  wt.DWORD),
        ("hEvent",      wt.HANDLE),
    ]

received = []
apc_event = threading.Event()

def on_read_complete(error_code, bytes_transferred, p_overlapped):
    """APC callback - fired when ReadFileEx IRP completes."""
    received.append((error_code, bytes_transferred))
    print(f"APC fired! error={error_code}, bytes={bytes_transferred}", flush=True)
    apc_event.set()

callback = LPOVERLAPPED_COMPLETION_ROUTINE(on_read_complete)

# Open the COM port
h = k.CreateFileW(f"\\\\.\\{PORT}", 0xC0000000, 3, None, 3, 0x40000000, None)
if ctypes.get_last_error() != 0 or h in (wt.HANDLE(-1).value, 0):
    print(f"ERROR: CreateFile failed: {ctypes.get_last_error()}", flush=True)
    sys.exit(1)

print(f"OPEN: {PORT} handle={h}", flush=True)

# Issue ReadFileEx for 1 byte
buf = (ctypes.c_char * 64)()
ov = OVERLAPPED()
ok = k.ReadFileEx(h, buf, 1, ctypes.byref(ov), callback)
err = ctypes.get_last_error()
print(f"ReadFileEx: ok={ok}, err={err}", flush=True)

if not ok and err != 997:  # 997=ERROR_IO_PENDING is ok
    print(f"ReadFileEx FAILED: err={err}", flush=True)
    k.CloseHandle(h)
    sys.exit(1)

# Enter alertable wait — APC will fire here when data arrives
print("Entering SleepEx (alertable, 8s timeout)...", flush=True)
result = k.SleepEx(8000, 1)  # alertable=TRUE
print(f"SleepEx returned: {result} (192=APC, 0=timeout)", flush=True)

if received:
    error_code, nbytes = received[0]
    data = buf.raw[:nbytes]
    print(f"RECEIVED: {nbytes} bytes: {data!r}", flush=True)

    if nbytes > 0:
        # Issue MAXDWORD ReadFile for remaining bytes
        ov2 = OVERLAPPED()
        ov2.hEvent = k.CreateEventW(None, True, False, None)
        remaining = (ctypes.c_char * 64)()
        n2 = wt.DWORD(0)

        # Set MAXDWORD timeout
        class COMMTIMEOUTS(ctypes.Structure):
            _fields_ = [("ReadIntervalTimeout", wt.DWORD), ("ReadTotalTimeoutMultiplier", wt.DWORD),
                        ("ReadTotalTimeoutConstant", wt.DWORD), ("WriteTotalTimeoutMultiplier", wt.DWORD),
                        ("WriteTotalTimeoutConstant", wt.DWORD)]
        ct = COMMTIMEOUTS(0xFFFFFFFF, 0, 0, 0, 0)
        k.SetCommTimeouts(h, ctypes.byref(ct))

        read_ok = k.ReadFile(h, remaining, 64, ctypes.byref(n2), ctypes.byref(ov2))
        err2 = ctypes.get_last_error()
        print(f"MAXDWORD ReadFile: ok={read_ok}, err={err2}", flush=True)

        if not read_ok and err2 == 997:
            # Wait for it
            wait_res = k.WaitForSingleObject(ov2.hEvent, 2000)
            print(f"WaitForSingleObject: {wait_res}", flush=True)
            if wait_res == 0:
                got_ok = k.GetOverlappedResult(h, ctypes.byref(ov2), ctypes.byref(n2), False)
                print(f"GetOverlappedResult: ok={got_ok}, n={n2.value}", flush=True)

        total = nbytes + n2.value
        all_data = data + remaining.raw[:n2.value]
        print(f"TOTAL: {total} bytes: {all_data!r}", flush=True)
        k.CloseHandle(ov2.hEvent)
else:
    print("TIMEOUT: No APC received", flush=True)

k.CloseHandle(h)
print("DONE", flush=True)
