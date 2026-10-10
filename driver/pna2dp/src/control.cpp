/*
 * control.cpp - private control interface dispatch (create/cleanup/close/ioctl).
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

#define PN_CTL_FILE_MAGIC   ((PVOID)(ULONG_PTR)0x50414332u)   /* 'PAC2' */

BOOLEAN ControlIsOurFile(_In_ PFILE_OBJECT File)
{
    return File && File->FsContext2 == PN_CTL_FILE_MAGIC;
}

NTSTATUS ControlRegisterInterface(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    UNICODE_STRING ref;
    RtlInitUnicodeString(&ref, PNA2DP_CONTROL_REFSTRING);
    NTSTATUS status = IoRegisterDeviceInterface(D->Pdo, &GUID_DEVINTERFACE_PNA2DP, &ref, &D->InterfaceSymlink);
    if (!NT_SUCCESS(status)) { PN_WARN("IoRegisterDeviceInterface failed 0x%08x", status); return status; }
    status = IoSetDeviceInterfaceState(&D->InterfaceSymlink, TRUE);
    if (!NT_SUCCESS(status)) { PN_WARN("IoSetDeviceInterfaceState failed 0x%08x", status); RtlFreeUnicodeString(&D->InterfaceSymlink); return status; }
    D->InterfaceRegistered = TRUE;
    PN_INFO("control interface %wZ", &D->InterfaceSymlink);
    return STATUS_SUCCESS;
}

VOID ControlUnregisterInterface(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    if (D->InterfaceRegistered) {
        IoSetDeviceInterfaceState(&D->InterfaceSymlink, FALSE);
        RtlFreeUnicodeString(&D->InterfaceSymlink);
        D->InterfaceRegistered = FALSE;
    }
}

static BOOLEAN IsControlOpen(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PFILE_OBJECT f = sp->FileObject;
    if (!f) return FALSE;
    /* FileName is "\pna2dpctl" when opened through the device interface with our reference string */
    UNICODE_STRING ref;
    RtlInitUnicodeString(&ref, L"\\" PNA2DP_CONTROL_REFSTRING);
    return RtlEqualUnicodeString(&f->FileName, &ref, TRUE);
}

static NTSTATUS CompleteIrp(_In_ PIRP Irp, _In_ NTSTATUS Status, _In_ ULONG_PTR Info)
{
    Irp->IoStatus.Status = Status;
    Irp->IoStatus.Information = Info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return Status;
}

NTSTATUS ControlDispatchCreate(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    if (!IsControlOpen(Irp)) return PcDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = DeviceFromFdo(DeviceObject);
    if (!D || D->Removing) return CompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    sp->FileObject->FsContext2 = PN_CTL_FILE_MAGIC;
    sp->FileObject->FsContext = D;
    InterlockedIncrement(&D->OpenHandles);
    return CompleteIrp(Irp, STATUS_SUCCESS, FILE_OPENED);
}

NTSTATUS ControlDispatchCleanup(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PcDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    if (D && D->Signature == PNA2DP_DEVICE_SIGNATURE) {
        DeviceFlushWaitersForFile(D, sp->FileObject);
        CodecHostFlushFile(&D->Host, sp->FileObject);
    }
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS ControlDispatchClose(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PAGED_CODE();
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PcDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    if (D && D->Signature == PNA2DP_DEVICE_SIGNATURE) InterlockedDecrement(&D->OpenHandles);
    sp->FileObject->FsContext2 = NULL;
    sp->FileObject->FsContext = NULL;
    return CompleteIrp(Irp, STATUS_SUCCESS, 0);
}

NTSTATUS ControlDispatchDeviceControl(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (!ControlIsOurFile(sp->FileObject)) return PcDispatchIrp(DeviceObject, Irp);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)sp->FileObject->FsContext;
    if (!D || D->Signature != PNA2DP_DEVICE_SIGNATURE || D->Removing) return CompleteIrp(Irp, STATUS_DEVICE_NOT_READY, 0);

    ULONG code = sp->Parameters.DeviceIoControl.IoControlCode;
    ULONG inLen = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    PVOID buf = Irp->AssociatedIrp.SystemBuffer;
    NTSTATUS status = STATUS_INVALID_DEVICE_REQUEST;
    ULONG_PTR info = 0;

    if (METHOD_FROM_CTL_CODE(code) != METHOD_BUFFERED) return CompleteIrp(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);

    switch (code) {
    case IOCTL_PNA2DP_GET_VERSION: {
        if (outLen < sizeof(PNA2DP_VERSION)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PNA2DP_VERSION *v = (PNA2DP_VERSION *)buf;
        RtlZeroMemory(v, sizeof(*v));
        v->AbiVersion = PNA2DP_ABI_VERSION;
        v->DriverVersion = PNA2DP_DRIVER_VERSION;
        v->Role = 1;
        v->KernelCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
        v->HostCodecMask = CodecHostCodecMask(&D->Host);
        info = sizeof(*v);
        status = STATUS_SUCCESS;
        break;
    }
    case IOCTL_PNA2DP_GET_STATUS:
        if (outLen < sizeof(PNA2DP_STATUS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetStatus(D, (PPNA2DP_STATUS)buf);
        info = sizeof(PNA2DP_STATUS);
        break;
    case IOCTL_PNA2DP_GET_CAPABILITIES:
        if (outLen < sizeof(PNA2DP_CAPABILITIES)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetCapabilities(D, (PPNA2DP_CAPABILITIES)buf);
        info = sizeof(PNA2DP_CAPABILITIES);
        break;
    case IOCTL_PNA2DP_GET_CONFIG:
        if (outLen < sizeof(PNA2DP_CONFIG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceGetConfig(D, (PPNA2DP_CONFIG)buf);
        info = sizeof(PNA2DP_CONFIG);
        break;
    case IOCTL_PNA2DP_SET_CONFIG:
        if (inLen < sizeof(PNA2DP_CONFIG)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = DeviceSetConfig(D, (const PNA2DP_CONFIG *)buf);
        break;
    case IOCTL_PNA2DP_WAIT_EVENT:
        status = DeviceQueueWaitEvent(D, Irp);
        if (status == STATUS_PENDING) { IoMarkIrpPending(Irp); return STATUS_PENDING; }
        if (NT_SUCCESS(status)) info = Irp->IoStatus.Information;
        break;
    case IOCTL_PNA2DP_RECONNECT:
        status = DeviceReconnect(D);
        break;
    case IOCTL_PNA2DP_GET_STATS:
        if (outLen < sizeof(PNA2DP_STATS)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        PumpGetStats(&D->Pump, (PPNA2DP_STATS)buf);
        info = sizeof(PNA2DP_STATS);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_PNA2DP_RESET_STATS:
        PumpResetStats(&D->Pump);
        status = STATUS_SUCCESS;
        break;

    case IOCTL_PNA2DP_CODEC_REGISTER:
        if (inLen < sizeof(PNA2DP_CODEC_REGISTER)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        status = CodecHostRegister(&D->Host, sp->FileObject, (const PNA2DP_CODEC_REGISTER *)buf);
        break;
    case IOCTL_PNA2DP_CODEC_UNREGISTER:
        CodecHostUnregister(&D->Host, sp->FileObject);
        status = STATUS_SUCCESS;
        break;
    case IOCTL_PNA2DP_CODEC_GET_WORK:
        status = CodecHostQueueGetWork(&D->Host, Irp);
        if (status == STATUS_PENDING) { IoMarkIrpPending(Irp); return STATUS_PENDING; }
        break;
    case IOCTL_PNA2DP_CODEC_PUT_RESULT: {
        if (inLen < sizeof(PNA2DP_CODEC_RESULT)) { status = STATUS_BUFFER_TOO_SMALL; break; }
        const PNA2DP_CODEC_RESULT *r = (const PNA2DP_CODEC_RESULT *)buf;
        if (r->Size != inLen || r->PayloadBytes + sizeof(*r) > inLen) { status = STATUS_INVALID_PARAMETER; break; }
        status = PumpHostResult(&D->Pump, r, (const UCHAR *)(r + 1));
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }
    return CompleteIrp(Irp, status, NT_SUCCESS(status) ? info : 0);
}
