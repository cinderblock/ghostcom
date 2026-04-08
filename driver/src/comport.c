/*
 * comport.c — COM port device: serial IOCTL handling + data I/O.
 *
 * This device (\\.\COM<N>) is what external applications open. It
 * implements the full Windows serial port interface (IOCTL_SERIAL_*).
 *
 * Data written here flows into the ComToCompanion ring buffer.
 * Data readable here comes from the CompanionToCom ring buffer.
 * Serial IOCTLs update the port pair's signal state and notify
 * companion-side waiters.
 */

#include "driver.h"

/* ── Tracing ──────────────────────────────────────────────────── */

#define TraceEvents(level, flag, msg, ...) \
    KdPrintEx((DPFLTR_DEFAULT_ID, DPFLTR_INFO_LEVEL, "node-null [COM]: " msg "\n", ##__VA_ARGS__))


/* ── File create / close callbacks ────────────────────────────── */

VOID
VcomComEvtFileCreate(
    _In_ WDFDEVICE     Device,
    _In_ WDFREQUEST    Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(Device);

    PVCOM_PORT_DEVICE_CTX devCtx = VcomGetPortDeviceContext(Device);
    PVCOM_PORT_PAIR pp = devCtx->PortPair;

    /* Set up the file context so I/O handlers can find the port pair. */
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(FileObject);
    fileCtx->FileType = VcomFileTypeCom;
    fileCtx->PortPair = pp;

    InterlockedIncrement(&pp->ComSideOpen);

    /* Notify the companion side that the COM port was opened. */
    VcomSignalChanged(pp, VCOM_CHANGED_COM_OPEN);

    TraceEvents(0, 0, "COM%lu opened (count=%ld)", pp->PortNumber, pp->ComSideOpen);

    WdfRequestComplete(Request, STATUS_SUCCESS);
}

VOID
VcomComEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    PVCOM_FILE_CTX fileCtx = VcomGetFileContext(FileObject);
    PVCOM_PORT_PAIR pp = fileCtx->PortPair;

    if (pp) {
        InterlockedDecrement(&pp->ComSideOpen);
        VcomSignalChanged(pp, VCOM_CHANGED_COM_CLOSE);
        TraceEvents(0, 0, "COM%lu closed (count=%ld)", pp->PortNumber, pp->ComSideOpen);
    }
}


/* ── COM port device creation ─────────────────────────────────── */

NTSTATUS
VcomComPortCreate(
    _In_ WDFDRIVER Driver,
    _In_ PVCOM_DEVICE_CTX DevCtx,
    _In_ PVCOM_PORT_PAIR PortPair,
    _In_ ULONG PortNumber
)
{
    NTSTATUS status;
    PWDFDEVICE_INIT deviceInit;
    WDFDEVICE comDevice;
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

    /* Assign the device name: \Device\VCOMSerial<N> */
    WCHAR deviceNameBuf[64];
    UNICODE_STRING deviceName;
    RtlStringCbPrintfW(deviceNameBuf, sizeof(deviceNameBuf),
                       L"\\Device\\VCOMSerial%lu", PortNumber);
    RtlInitUnicodeString(&deviceName, deviceNameBuf);

    status = WdfDeviceInitAssignName(deviceInit, &deviceName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);

    /* Configure file object support (for tracking open/close). */
    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig,
                               VcomComEvtFileCreate,
                               VcomComEvtFileClose,
                               WDF_NO_EVENT_CALLBACK);

    WDF_OBJECT_ATTRIBUTES fileAttributes;
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&fileAttributes, VCOM_FILE_CTX);
    WdfDeviceInitSetFileObjectConfig(deviceInit, &fileConfig, &fileAttributes);

    /* ── Create the device ──────────────────────────────────── */

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, VCOM_PORT_DEVICE_CTX);

    status = WdfDeviceCreate(&deviceInit, &attributes, &comDevice);
    if (!NT_SUCCESS(status)) {
        /* deviceInit is freed on failure */
        return status;
    }

    /* Store port pair reference in the device context. */
    PVCOM_PORT_DEVICE_CTX portDevCtx = VcomGetPortDeviceContext(comDevice);
    portDevCtx->PortPair = PortPair;
    portDevCtx->IsComSide = TRUE;

    PortPair->ComDevice = comDevice;

    /* ── Create the default I/O queue ───────────────────────── */

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig,
                                            WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = VcomComEvtRead;
    queueConfig.EvtIoWrite = VcomComEvtWrite;
    queueConfig.EvtIoDeviceControl = VcomComEvtIoctl;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = comDevice;

    WDFQUEUE defaultQueue;
    status = WdfIoQueueCreate(comDevice, &queueConfig, &attributes, &defaultQueue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM default queue create failed: 0x%08X", status);
        return status;
    }

    /* ── Create manual queues for pending I/O ───────────────── */

    /* Pending COM reads (waiting for companion to write data). */
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = comDevice;

    status = WdfIoQueueCreate(comDevice, &queueConfig, &attributes,
                               &PortPair->ComReadQueue);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM read queue create failed: 0x%08X", status);
        return status;
    }

    /* Pending COM writes (when ring buffer is full). */
    status = WdfIoQueueCreate(comDevice, &queueConfig, &attributes,
                               &PortPair->ComWriteQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* Pending WaitCommEvent requests. */
    status = WdfIoQueueCreate(comDevice, &queueConfig, &attributes,
                               &PortPair->WaitMaskQueue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* ── Create symbolic link: \DosDevices\COM<N> ───────────── */

    WCHAR symLinkBuf[64];
    RtlStringCbPrintfW(symLinkBuf, sizeof(symLinkBuf),
                       L"\\DosDevices\\COM%lu", PortNumber);

    PortPair->ComSymLink.Buffer = (PWCHAR)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, sizeof(symLinkBuf), VCOM_POOL_TAG);
    if (!PortPair->ComSymLink.Buffer) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlCopyMemory(PortPair->ComSymLink.Buffer, symLinkBuf, sizeof(symLinkBuf));
    PortPair->ComSymLink.Length = (USHORT)(wcslen(symLinkBuf) * sizeof(WCHAR));
    PortPair->ComSymLink.MaximumLength = sizeof(symLinkBuf);

    status = WdfDeviceCreateSymbolicLink(comDevice, &PortPair->ComSymLink);
    if (!NT_SUCCESS(status)) {
        TraceEvents(0, 0, "COM%lu symlink create failed: 0x%08X",
                    PortNumber, status);
        ExFreePoolWithTag(PortPair->ComSymLink.Buffer, VCOM_POOL_TAG);
        PortPair->ComSymLink.Buffer = NULL;
        return status;
    }

    /* ── Register in SERIALCOMM device map ──────────────────── */

    WCHAR valueNameBuf[64];
    WCHAR valueDataBuf[16];
    UNICODE_STRING valueName, valueData;
    UNICODE_STRING registryPath;

    RtlStringCbPrintfW(valueNameBuf, sizeof(valueNameBuf),
                       L"\\Device\\VCOMSerial%lu", PortNumber);
    RtlInitUnicodeString(&valueName, valueNameBuf);

    RtlStringCbPrintfW(valueDataBuf, sizeof(valueDataBuf),
                       L"COM%lu", PortNumber);
    RtlInitUnicodeString(&valueData, valueDataBuf);

    RtlInitUnicodeString(&registryPath,
                         L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\SERIALCOMM");

    OBJECT_ATTRIBUTES keyAttr;
    HANDLE keyHandle;
    InitializeObjectAttributes(&keyAttr, &registryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    status = ZwCreateKey(&keyHandle, KEY_SET_VALUE, &keyAttr, 0, NULL,
                         REG_OPTION_VOLATILE, NULL);
    if (NT_SUCCESS(status)) {
        ZwSetValueKey(keyHandle, &valueName, 0, REG_SZ,
                      valueData.Buffer,
                      valueData.Length + sizeof(WCHAR));
        ZwClose(keyHandle);
    }

    /* ── Finish initialization ──────────────────────────────── */

    WdfControlFinishInitializing(comDevice);

    TraceEvents(0, 0, "COM port device created: COM%lu (\\Device\\VCOMSerial%lu)",
                PortNumber, PortNumber);

    return STATUS_SUCCESS;
}


/* ── COM port destruction ─────────────────────────────────────── */

VOID
VcomComPortDestroy(
    _In_ PVCOM_PORT_PAIR PortPair
)
{
    /* Remove from SERIALCOMM device map. */
    WCHAR valueNameBuf[64];
    UNICODE_STRING valueName, registryPath;

    RtlStringCbPrintfW(valueNameBuf, sizeof(valueNameBuf),
                       L"\\Device\\VCOMSerial%lu", PortPair->PortNumber);
    RtlInitUnicodeString(&valueName, valueNameBuf);

    RtlInitUnicodeString(&registryPath,
                         L"\\Registry\\Machine\\HARDWARE\\DEVICEMAP\\SERIALCOMM");

    OBJECT_ATTRIBUTES keyAttr;
    HANDLE keyHandle;
    InitializeObjectAttributes(&keyAttr, &registryPath,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
                               NULL, NULL);

    if (NT_SUCCESS(ZwOpenKey(&keyHandle, KEY_SET_VALUE, &keyAttr))) {
        ZwDeleteValueKey(keyHandle, &valueName);
        ZwClose(keyHandle);
    }

    /* Free the symbolic link string buffer. */
    if (PortPair->ComSymLink.Buffer) {
        ExFreePoolWithTag(PortPair->ComSymLink.Buffer, VCOM_POOL_TAG);
        PortPair->ComSymLink.Buffer = NULL;
    }

    /* Delete the WDF device (this also removes the symbolic link). */
    if (PortPair->ComDevice) {
        WdfObjectDelete(PortPair->ComDevice);
        PortPair->ComDevice = NULL;
    }
}


/* ── COM-side Read ────────────────────────────────────────────── */

VOID
VcomComEvtRead(
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

    /* Try to read from CompanionToCom ring buffer. */
    PVOID outputBuf;
    size_t outputLen;
    NTSTATUS st = WdfRequestRetrieveOutputBuffer(Request, 1, &outputBuf, &outputLen);
    if (!NT_SUCCESS(st)) {
        WdfRequestComplete(Request, st);
        return;
    }

    ULONG bytesRead = VcomRingRead(&pp->CompanionToCom,
                                    (PUCHAR)outputBuf,
                                    (ULONG)min(outputLen, Length));

    if (bytesRead > 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesRead);

        /* Unblock any pending companion writes. */
        if (pp->CompWriteQueue) {
            VcomDrainWritesToRing(&pp->CompanionToCom,
                                  pp->CompWriteQueue,
                                  pp->ComReadQueue);
        }

        pp->PerfStats.ReceivedCount += bytesRead;
    } else {
        /* No data available — pend the request. */
        st = WdfRequestForwardToIoQueue(Request, pp->ComReadQueue);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── COM-side Write ───────────────────────────────────────────── */

VOID
VcomComEvtWrite(
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

    ULONG bytesWritten = VcomRingWrite(&pp->ComToCompanion,
                                        (const PUCHAR)inputBuf,
                                        (ULONG)min(inputLen, Length));

    if (bytesWritten > 0) {
        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, bytesWritten);

        /* Wake up pending companion reads. */
        if (pp->CompReadQueue) {
            VcomDrainRingToReads(&pp->ComToCompanion, pp->CompReadQueue);
        }

        /* Notify WaitCommEvent (EV_TXEMPTY when ring drains). */
        if (VcomRingIsEmpty(&pp->ComToCompanion)) {
            VcomCheckWaitMask(pp, SERIAL_EV_TXEMPTY);
        }

        pp->PerfStats.TransmittedCount += bytesWritten;
    } else {
        /* Ring buffer full — pend the write. */
        st = WdfRequestForwardToIoQueue(Request, pp->ComWriteQueue);
        if (!NT_SUCCESS(st)) {
            WdfRequestComplete(Request, st);
        }
    }
}


/* ── COM-side IOCTL handler ───────────────────────────────────── */

VOID
VcomComEvtIoctl(
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

    /* ── Baud Rate ──────────────────────────────────────────── */

    case IOCTL_SERIAL_SET_BAUD_RATE:
    {
        PSERIAL_BAUD_RATE br;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SERIAL_BAUD_RATE),
                                                (PVOID*)&br, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.BaudRate = br->BaudRate;
            WdfSpinLockRelease(pp->SignalLock);
            VcomSignalChanged(pp, VCOM_CHANGED_BAUD);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_BAUD_RATE:
    {
        PSERIAL_BAUD_RATE br;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_BAUD_RATE),
                                                 (PVOID*)&br, NULL);
        if (NT_SUCCESS(status)) {
            br->BaudRate = pp->SignalState.BaudRate;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_BAUD_RATE));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Line Control (data bits, stop bits, parity) ────────── */

    case IOCTL_SERIAL_SET_LINE_CONTROL:
    {
        PSERIAL_LINE_CONTROL lc;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SERIAL_LINE_CONTROL),
                                                (PVOID*)&lc, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.StopBits = lc->StopBits;
            pp->SignalState.Parity = lc->Parity;
            pp->SignalState.DataBits = lc->WordLength;
            WdfSpinLockRelease(pp->SignalLock);
            VcomSignalChanged(pp, VCOM_CHANGED_LINE_CTRL);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_LINE_CONTROL:
    {
        PSERIAL_LINE_CONTROL lc;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_LINE_CONTROL),
                                                 (PVOID*)&lc, NULL);
        if (NT_SUCCESS(status)) {
            lc->StopBits = pp->SignalState.StopBits;
            lc->Parity = pp->SignalState.Parity;
            lc->WordLength = pp->SignalState.DataBits;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_LINE_CONTROL));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── DTR / RTS ──────────────────────────────────────────── */

    case IOCTL_SERIAL_SET_DTR:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.DtrState = TRUE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_DTR);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_SERIAL_CLR_DTR:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.DtrState = FALSE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_DTR);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_SERIAL_SET_RTS:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.RtsState = TRUE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_RTS);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_SERIAL_CLR_RTS:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.RtsState = FALSE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_RTS);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    /* ── Break ──────────────────────────────────────────────── */

    case IOCTL_SERIAL_SET_BREAK_ON:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.BreakState = TRUE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_BREAK);
        VcomCheckWaitMask(pp, SERIAL_EV_BREAK);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    case IOCTL_SERIAL_SET_BREAK_OFF:
        WdfSpinLockAcquire(pp->SignalLock);
        pp->SignalState.BreakState = FALSE;
        WdfSpinLockRelease(pp->SignalLock);
        VcomSignalChanged(pp, VCOM_CHANGED_BREAK);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    /* ── Timeouts ───────────────────────────────────────────── */

    case IOCTL_SERIAL_SET_TIMEOUTS:
    {
        PSERIAL_TIMEOUTS to;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SERIAL_TIMEOUTS),
                                                (PVOID*)&to, NULL);
        if (NT_SUCCESS(status)) {
            pp->Timeouts = *to;
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_TIMEOUTS:
    {
        PSERIAL_TIMEOUTS to;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_TIMEOUTS),
                                                 (PVOID*)&to, NULL);
        if (NT_SUCCESS(status)) {
            *to = pp->Timeouts;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_TIMEOUTS));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Handshake / Flow Control ───────────────────────────── */

    case IOCTL_SERIAL_SET_HANDFLOW:
    {
        PSERIAL_HANDFLOW hf;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SERIAL_HANDFLOW),
                                                (PVOID*)&hf, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.ControlHandShake = hf->ControlHandShake;
            pp->SignalState.FlowReplace = hf->FlowReplace;
            pp->SignalState.XonLimit = hf->XonLimit;
            pp->SignalState.XoffLimit = hf->XoffLimit;
            WdfSpinLockRelease(pp->SignalLock);
            VcomSignalChanged(pp, VCOM_CHANGED_HANDFLOW);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_HANDFLOW:
    {
        PSERIAL_HANDFLOW hf;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_HANDFLOW),
                                                 (PVOID*)&hf, NULL);
        if (NT_SUCCESS(status)) {
            hf->ControlHandShake = pp->SignalState.ControlHandShake;
            hf->FlowReplace = pp->SignalState.FlowReplace;
            hf->XonLimit = pp->SignalState.XonLimit;
            hf->XoffLimit = pp->SignalState.XoffLimit;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_HANDFLOW));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Special Characters ─────────────────────────────────── */

    case IOCTL_SERIAL_SET_CHARS:
    {
        PSERIAL_CHARS ch;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(SERIAL_CHARS),
                                                (PVOID*)&ch, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.EofChar = ch->EofChar;
            pp->SignalState.ErrorChar = ch->ErrorChar;
            pp->SignalState.BreakChar = ch->BreakChar;
            pp->SignalState.EventChar = ch->EventChar;
            pp->SignalState.XonChar = ch->XonChar;
            pp->SignalState.XoffChar = ch->XoffChar;
            WdfSpinLockRelease(pp->SignalLock);
            VcomSignalChanged(pp, VCOM_CHANGED_CHARS);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_CHARS:
    {
        PSERIAL_CHARS ch;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_CHARS),
                                                 (PVOID*)&ch, NULL);
        if (NT_SUCCESS(status)) {
            ch->EofChar = pp->SignalState.EofChar;
            ch->ErrorChar = pp->SignalState.ErrorChar;
            ch->BreakChar = pp->SignalState.BreakChar;
            ch->EventChar = pp->SignalState.EventChar;
            ch->XonChar = pp->SignalState.XonChar;
            ch->XoffChar = pp->SignalState.XoffChar;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_CHARS));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── WaitCommEvent ──────────────────────────────────────── */

    case IOCTL_SERIAL_SET_WAIT_MASK:
    {
        PULONG mask;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG),
                                                (PVOID*)&mask, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.WaitMask = *mask;
            WdfSpinLockRelease(pp->SignalLock);

            /* Cancel any pending WAIT_ON_MASK requests — per spec,
             * setting a new mask cancels previous waits. */
            if (pp->WaitMaskQueue) {
                WdfIoQueuePurge(pp->WaitMaskQueue, NULL, NULL);
            }

            VcomSignalChanged(pp, VCOM_CHANGED_WAIT_MASK);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_GET_WAIT_MASK:
    {
        PULONG mask;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG),
                                                 (PVOID*)&mask, NULL);
        if (NT_SUCCESS(status)) {
            *mask = pp->SignalState.WaitMask;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_WAIT_ON_MASK:
    {
        /* Pend this request until a matching event occurs. */
        status = WdfRequestForwardToIoQueue(Request, pp->WaitMaskQueue);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Modem Status (null-modem crossover) ────────────────── */

    case IOCTL_SERIAL_GET_MODEMSTATUS:
    {
        PULONG modemStatus;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG),
                                                 (PVOID*)&modemStatus, NULL);
        if (NT_SUCCESS(status)) {
            ULONG ms = 0;
            if (pp->CompDtr) {
                ms |= SERIAL_DSR_STATE | SERIAL_DCD_STATE;
            }
            if (pp->CompRts) {
                ms |= SERIAL_CTS_STATE;
            }
            *modemStatus = ms;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── COMM Status ────────────────────────────────────────── */

    case IOCTL_SERIAL_GET_COMMSTATUS:
    {
        PSERIAL_STATUS ss;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_STATUS),
                                                 (PVOID*)&ss, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(ss, sizeof(SERIAL_STATUS));
            ss->AmountInInQueue = VcomRingReadAvailable(&pp->CompanionToCom);
            ss->AmountInOutQueue = VcomRingReadAvailable(&pp->ComToCompanion);
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_STATUS));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Properties ─────────────────────────────────────────── */

    case IOCTL_SERIAL_GET_PROPERTIES:
    {
        PSERIAL_COMMPROP props;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIAL_COMMPROP),
                                                 (PVOID*)&props, NULL);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(props, sizeof(SERIAL_COMMPROP));
            props->PacketLength = sizeof(SERIAL_COMMPROP);
            props->PacketVersion = 2;
            props->ServiceMask = SERIAL_SP_SERIALCOMM;
            props->MaxTxQueue = VCOM_RING_BUFFER_SIZE;
            props->MaxRxQueue = VCOM_RING_BUFFER_SIZE;
            props->MaxBaud = SERIAL_BAUD_USER;
            props->ProvSubType = SERIAL_SP_RS232;
            props->ProvCapabilities =
                SERIAL_PCF_DTRDSR | SERIAL_PCF_RTSCTS |
                SERIAL_PCF_CD | SERIAL_PCF_PARITY_CHECK |
                SERIAL_PCF_XONXOFF | SERIAL_PCF_SETXCHAR |
                SERIAL_PCF_TOTALTIMEOUTS | SERIAL_PCF_INTTIMEOUTS;
            props->SettableParams =
                SERIAL_SP_PARITY | SERIAL_SP_BAUD |
                SERIAL_SP_DATABITS | SERIAL_SP_STOPBITS |
                SERIAL_SP_HANDSHAKING | SERIAL_SP_PARITY_CHECK |
                SERIAL_SP_CARRIER_DETECT;
            props->SettableBaud =
                SERIAL_BAUD_075 | SERIAL_BAUD_110 | SERIAL_BAUD_150 |
                SERIAL_BAUD_300 | SERIAL_BAUD_600 | SERIAL_BAUD_1200 |
                SERIAL_BAUD_1800 | SERIAL_BAUD_2400 | SERIAL_BAUD_4800 |
                SERIAL_BAUD_7200 | SERIAL_BAUD_9600 | SERIAL_BAUD_14400 |
                SERIAL_BAUD_19200 | SERIAL_BAUD_38400 | SERIAL_BAUD_56K |
                SERIAL_BAUD_57600 | SERIAL_BAUD_115200 | SERIAL_BAUD_128K |
                SERIAL_BAUD_USER;
            props->SettableData =
                SERIAL_DATABITS_5 | SERIAL_DATABITS_6 |
                SERIAL_DATABITS_7 | SERIAL_DATABITS_8;
            props->SettableStopParity =
                SERIAL_STOPBITS_10 | SERIAL_STOPBITS_15 | SERIAL_STOPBITS_20 |
                SERIAL_PARITY_NONE | SERIAL_PARITY_ODD | SERIAL_PARITY_EVEN |
                SERIAL_PARITY_MARK | SERIAL_PARITY_SPACE;
            props->CurrentTxQueue = VCOM_RING_BUFFER_SIZE;
            props->CurrentRxQueue = VCOM_RING_BUFFER_SIZE;

            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIAL_COMMPROP));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Purge ──────────────────────────────────────────────── */

    case IOCTL_SERIAL_PURGE:
    {
        PULONG flags;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG),
                                                (PVOID*)&flags, NULL);
        if (NT_SUCCESS(status)) {
            if (*flags & SERIAL_PURGE_TXABORT && pp->ComWriteQueue) {
                WdfIoQueuePurge(pp->ComWriteQueue, NULL, NULL);
            }
            if (*flags & SERIAL_PURGE_RXABORT && pp->ComReadQueue) {
                WdfIoQueuePurge(pp->ComReadQueue, NULL, NULL);
            }
            if (*flags & SERIAL_PURGE_TXCLEAR) {
                VcomRingFlush(&pp->ComToCompanion);
            }
            if (*flags & SERIAL_PURGE_RXCLEAR) {
                VcomRingFlush(&pp->CompanionToCom);
            }
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Queue Size ─────────────────────────────────────────── */

    case IOCTL_SERIAL_SET_QUEUE_SIZE:
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    /* ── DTR/RTS query ──────────────────────────────────────── */

    case IOCTL_SERIAL_GET_DTRRTS:
    {
        PULONG dtrrts;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG),
                                                 (PVOID*)&dtrrts, NULL);
        if (NT_SUCCESS(status)) {
            ULONG val = 0;
            if (pp->SignalState.DtrState) val |= SERIAL_DTR_STATE;
            if (pp->SignalState.RtsState) val |= SERIAL_RTS_STATE;
            *dtrrts = val;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Statistics ──────────────────────────────────────────── */

    case IOCTL_SERIAL_GET_STATS:
    {
        PSERIALPERF_STATS stats;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(SERIALPERF_STATS),
                                                 (PVOID*)&stats, NULL);
        if (NT_SUCCESS(status)) {
            *stats = pp->PerfStats;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(SERIALPERF_STATS));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_CLEAR_STATS:
        RtlZeroMemory(&pp->PerfStats, sizeof(SERIALPERF_STATS));
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    /* ── No-op IOCTLs (virtual port has no physical hardware) ── */

    case IOCTL_SERIAL_SET_XOFF:
    case IOCTL_SERIAL_SET_XON:
    case IOCTL_SERIAL_RESET_DEVICE:
    case IOCTL_SERIAL_SET_FIFO_CONTROL:
    case IOCTL_SERIAL_LSRMST_INSERT:
    case IOCTL_SERIAL_XOFF_COUNTER:
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    /* ── Immediate Character ────────────────────────────────── */

    case IOCTL_SERIAL_IMMEDIATE_CHAR:
    {
        PUCHAR ch;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(UCHAR),
                                                (PVOID*)&ch, NULL);
        if (NT_SUCCESS(status)) {
            VcomRingWrite(&pp->ComToCompanion, ch, 1);
            if (pp->CompReadQueue) {
                VcomDrainRingToReads(&pp->ComToCompanion, pp->CompReadQueue);
            }
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Config size (for GetCommConfig) ────────────────────── */

    case IOCTL_SERIAL_CONFIG_SIZE:
    {
        PULONG configSize;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG),
                                                 (PVOID*)&configSize, NULL);
        if (NT_SUCCESS(status)) {
            *configSize = 0;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Modem Control Register ─────────────────────────────── */

    case IOCTL_SERIAL_GET_MODEM_CONTROL:
    {
        PULONG mcr;
        status = WdfRequestRetrieveOutputBuffer(Request, sizeof(ULONG),
                                                 (PVOID*)&mcr, NULL);
        if (NT_SUCCESS(status)) {
            ULONG val = 0;
            if (pp->SignalState.DtrState) val |= SERIAL_MCR_DTR;
            if (pp->SignalState.RtsState) val |= SERIAL_MCR_RTS;
            *mcr = val;
            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(ULONG));
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    case IOCTL_SERIAL_SET_MODEM_CONTROL:
    {
        PULONG mcr;
        status = WdfRequestRetrieveInputBuffer(Request, sizeof(ULONG),
                                                (PVOID*)&mcr, NULL);
        if (NT_SUCCESS(status)) {
            WdfSpinLockAcquire(pp->SignalLock);
            pp->SignalState.DtrState = (*mcr & SERIAL_MCR_DTR) ? TRUE : FALSE;
            pp->SignalState.RtsState = (*mcr & SERIAL_MCR_RTS) ? TRUE : FALSE;
            WdfSpinLockRelease(pp->SignalLock);
            VcomSignalChanged(pp, VCOM_CHANGED_DTR | VCOM_CHANGED_RTS);
            WdfRequestComplete(Request, STATUS_SUCCESS);
        } else {
            WdfRequestComplete(Request, status);
        }
        return;
    }

    /* ── Unknown IOCTL ──────────────────────────────────────── */

    default:
        TraceEvents(0, 0, "Unhandled COM IOCTL: 0x%08X", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}
