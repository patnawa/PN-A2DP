/*
 * codechost.cpp - inverted-call bridge to the user-mode codec host.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

/* ---- cancel-safe queue callbacks ------------------------------------------ */

static PN_CODEC_HOST *HostFromCsq(PIO_CSQ Csq) { return CONTAINING_RECORD(Csq, PN_CODEC_HOST, WorkCsq); }

static VOID CsqInsertIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    PN_CODEC_HOST *h = HostFromCsq(Csq);
    InsertTailList(&h->WorkQueue, &Irp->Tail.Overlay.ListEntry);
    InterlockedIncrement(&h->PendedCount);
}
static VOID CsqRemoveIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    PN_CODEC_HOST *h = HostFromCsq(Csq);
    RemoveEntryList(&Irp->Tail.Overlay.ListEntry);
    InterlockedDecrement(&h->PendedCount);
}
static PIRP CsqPeekNextIrp(_In_ PIO_CSQ Csq, _In_opt_ PIRP Irp, _In_opt_ PVOID PeekContext)
{
    PN_CODEC_HOST *h = HostFromCsq(Csq);
    PLIST_ENTRY e = Irp ? Irp->Tail.Overlay.ListEntry.Flink : h->WorkQueue.Flink;
    while (e != &h->WorkQueue) {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (!PeekContext) return irp;
        PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
        if (sp->FileObject == (PFILE_OBJECT)PeekContext) return irp;
        e = e->Flink;
    }
    return NULL;
}
_IRQL_raises_(DISPATCH_LEVEL) _IRQL_requires_max_(DISPATCH_LEVEL)
static VOID CsqAcquireLock(_In_ PIO_CSQ Csq, _Out_ PKIRQL Irql)
{
    KeAcquireSpinLock(&HostFromCsq(Csq)->WorkCsqLock, Irql);
}
_IRQL_requires_(DISPATCH_LEVEL)
static VOID CsqReleaseLock(_In_ PIO_CSQ Csq, _In_ KIRQL Irql)
{
    KeReleaseSpinLock(&HostFromCsq(Csq)->WorkCsqLock, Irql);
}
static VOID CsqCompleteCanceledIrp(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

/* ---- health / quarantine --------------------------------------------------- */

static ULONGLONG HostNowMs(void)
{
    return (ULONGLONG)PnNowHns() / PN_HNS_PER_MS;
}

/* Caller holds H->Lock. */
static VOID ArmQuarantineTimerLocked(_Inout_ PPN_CODEC_HOST H, _In_ ULONGLONG NowMs)
{
    if (H->Stopped || !H->Health.QuarantineUntilMs || H->Health.QuarantineUntilMs <= NowMs) return;
    ULONGLONG ms = PN_MIN(H->Health.QuarantineUntilMs - NowMs, 600000ull) + 1;
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)ms * PN_HNS_PER_MS;
    KeSetTimer(&H->QuarantineTimer, due, &H->QuarantineDpc);
}

/* DISPATCH: ends an expired quarantine and lets the device pick its codecs again. */
static KDEFERRED_ROUTINE QuarantineDpc;
static VOID QuarantineDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    PPN_CODEC_HOST H = (PPN_CODEC_HOST)Ctx;
    if (!H) return;
    KIRQL irql;
    BOOLEAN expired = FALSE;
    KeAcquireSpinLock(&H->Lock, &irql);
    if (!H->Stopped) {
        ULONGLONG now = HostNowMs();
        expired = PnHostHealthExpire(&H->Health, now);
        if (!expired) ArmQuarantineTimerLocked(H, now);   /* timer resolution: not quite due yet */
    }
    KeReleaseSpinLock(&H->Lock, irql);
    if (expired) {
        PN_INFO("codec host quarantine ended; re-evaluating codecs");
        DeviceHostChanged(H->Device);
    }
}

/* ---- API ------------------------------------------------------------------- */

/* Cannot fail; called from DeviceCreate so the queue is valid for every teardown path. */
NTSTATUS CodecHostInitialize(_Out_ PPN_CODEC_HOST H, _In_ PPNA2DP_DEVICE Device)
{
    RtlZeroMemory(H, sizeof(*H));
    H->Device = Device;
    KeInitializeTimer(&H->QuarantineTimer);
    KeInitializeDpc(&H->QuarantineDpc, QuarantineDpc, H);
    KeInitializeSpinLock(&H->Lock);
    KeInitializeSpinLock(&H->WorkCsqLock);
    InitializeListHead(&H->WorkQueue);
    return IoCsqInitialize(&H->WorkCsq, CsqInsertIrp, CsqRemoveIrp, CsqPeekNextIrp, CsqAcquireLock, CsqReleaseLock, CsqCompleteCanceledIrp);
}

VOID CodecHostUninitialize(_Inout_ PPN_CODEC_HOST H)
{
    if (!H->Device) return;   /* never initialized */
    KIRQL irql;
    KeAcquireSpinLock(&H->Lock, &irql);
    H->Stopped = TRUE;        /* no timer is armed after this */
    KeReleaseSpinLock(&H->Lock, irql);
    KeCancelTimer(&H->QuarantineTimer);
    KeFlushQueuedDpcs();      /* PASSIVE (DeviceStop): a DPC that was already queued has finished */
    CodecHostUnregister(H, NULL);
    for (;;) {
        PIRP irp = IoCsqRemoveNextIrp(&H->WorkCsq, NULL);
        if (!irp) break;
        irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

NTSTATUS CodecHostRegister(_Inout_ PPN_CODEC_HOST H, _In_ PFILE_OBJECT File, _In_ const PNA2DP_CODEC_REGISTER *Info)
{
    KIRQL irql;
    if (Info->Size != sizeof(*Info)) return STATUS_INVALID_PARAMETER;
    if (Info->AbiVersion != PNA2DP_ABI_VERSION) return STATUS_REVISION_MISMATCH;
    if (Info->MaxPcmBytes < 1024 || Info->MaxEncodedBytes < 256) return STATUS_INVALID_PARAMETER;
    KeAcquireSpinLock(&H->Lock, &irql);
    if (H->Registered && H->OwnerFile != File) {
        KeReleaseSpinLock(&H->Lock, irql);
        return STATUS_SHARING_VIOLATION;
    }
    H->Registered = TRUE;
    PnHostHealthRegistered(&H->Health);   /* a new host process gets a fresh chance */
    H->OwnerFile = File;
    H->Info = *Info;
    InterlockedIncrement(&H->Generation);
    KeReleaseSpinLock(&H->Lock, irql);
    PN_INFO("codec host registered: codecs 0x%08x pid %u", Info->CodecMask, Info->HostPid);
    DeviceHostChanged(H->Device);
    return STATUS_SUCCESS;
}

VOID CodecHostUnregister(_Inout_ PPN_CODEC_HOST H, _In_opt_ PFILE_OBJECT File)
{
    KIRQL irql;
    BOOLEAN was;
    KeAcquireSpinLock(&H->Lock, &irql);
    was = H->Registered && (File == NULL || H->OwnerFile == File);
    if (was) {
        H->Registered = FALSE;
        H->OwnerFile = NULL;
        RtlZeroMemory(&H->Info, sizeof(H->Info));
    }
    KeReleaseSpinLock(&H->Lock, irql);
    if (was) {
        PN_INFO("codec host unregistered");
        PumpHostLost(&H->Device->Pump);
        DeviceHostChanged(H->Device);
    }
}

ULONG CodecHostCodecMask(_In_ PPN_CODEC_HOST H)
{
    KIRQL irql;
    KeAcquireSpinLock(&H->Lock, &irql);
    ULONG mask = H->Registered && !PnHostHealthQuarantined(&H->Health, HostNowMs()) ? H->Info.CodecMask : 0;
    KeReleaseSpinLock(&H->Lock, irql);
    return mask;
}

BOOLEAN CodecHostAvailable(_In_ PPN_CODEC_HOST H, _In_ ULONG Codec)
{
    return (CodecHostCodecMask(H) & PNA2DP_CODEC_BIT(Codec)) != 0 && H->PendedCount > 0;
}

VOID CodecHostReportFailure(_Inout_ PPN_CODEC_HOST H)
{
    KIRQL irql;
    BOOLEAN started = FALSE;
    ULONG seconds = 0, quarantines = 0;
    KeAcquireSpinLock(&H->Lock, &irql);
    if (H->Registered && !H->Stopped) {
        ULONGLONG now = HostNowMs();
        started = PnHostHealthFailure(&H->Health, now);
        if (started) {
            seconds = PnHostQuarantineMs(H->Health.Quarantines) / 1000;
            quarantines = H->Health.Quarantines;
            ArmQuarantineTimerLocked(H, now);
        }
    }
    KeReleaseSpinLock(&H->Lock, irql);
    if (started) {
        PN_WARN("codec host failing repeatedly; quarantined for %u s, falling back", seconds);
        /* C9: once per quarantine start (the policy rate-limits them: 30 s, 2 min, 10 min) */
        PnLogDeviceEvent(H->Device->Fdo, PN_EVENT_CODEC_HOST, PN_EVENT_CODEC_HOST_QUARANTINE | (quarantines & 0xFFFF));
        DeviceHostChanged(H->Device);
    }
}

VOID CodecHostReportSuccess(_Inout_ PPN_CODEC_HOST H)
{
    KIRQL irql;
    KeAcquireSpinLock(&H->Lock, &irql);
    PnHostHealthSuccess(&H->Health, HostNowMs());
    KeReleaseSpinLock(&H->Lock, irql);
}

NTSTATUS CodecHostQueueGetWork(_Inout_ PPN_CODEC_HOST H, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(PNA2DP_CODEC_WORK) + 1024) return STATUS_BUFFER_TOO_SMALL;
    if (!H->Registered || H->OwnerFile != sp->FileObject) return STATUS_INVALID_DEVICE_STATE;
    PFILE_OBJECT file = sp->FileObject;   /* the IRP may be gone once it is queued */
    IoCsqInsertIrp(&H->WorkCsq, Irp, NULL);
    /* CLEANUP of this handle may have unregistered and flushed between the check and the
     * insert: flush again so no request of a closed handle stays queued */
    if (!H->Registered || H->OwnerFile != file) {
        for (;;) {
            PIRP irp = IoCsqRemoveNextIrp(&H->WorkCsq, file);
            if (!irp) break;
            irp->IoStatus.Status = STATUS_CANCELLED;
            irp->IoStatus.Information = 0;
            IoCompleteRequest(irp, IO_NO_INCREMENT);
        }
    } else {
        PumpHostWorkPended(&H->Device->Pump);   /* a PCM block may be waiting for this request */
    }
    return STATUS_PENDING;
}

VOID CodecHostFlushFile(_Inout_ PPN_CODEC_HOST H, _In_ PFILE_OBJECT File)
{
    if (!H->Device) return;
    CodecHostUnregister(H, File);   /* first, so a racing GET_WORK sees it and flushes itself */
    for (;;) {
        PIRP irp = IoCsqRemoveNextIrp(&H->WorkCsq, File);
        if (!irp) break;
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

NTSTATUS CodecHostDispatchWork(_Inout_ PPN_CODEC_HOST H, _In_ const PNA2DP_CODEC_WORK *Header,
                               _In_reads_bytes_opt_(PcmBytes) const UCHAR *Pcm, _In_ ULONG PcmBytes)
{
    PIRP irp = IoCsqRemoveNextIrp(&H->WorkCsq, NULL);
    if (!irp) return STATUS_DEVICE_NOT_READY;
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(irp);
    ULONG cap = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG need = sizeof(PNA2DP_CODEC_WORK) + PcmBytes;
    if (cap < need) {
        irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        return STATUS_BUFFER_TOO_SMALL;
    }
    PNA2DP_CODEC_WORK *out = (PNA2DP_CODEC_WORK *)irp->AssociatedIrp.SystemBuffer;
    *out = *Header;
    out->Size = need;
    out->PcmBytes = PcmBytes;
    if (PcmBytes && Pcm) RtlCopyMemory(out + 1, Pcm, PcmBytes);
    irp->IoStatus.Status = STATUS_SUCCESS;
    irp->IoStatus.Information = need;
    IoCompleteRequest(irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

PIRP CodecHostAcquireWork(_Inout_ PPN_CODEC_HOST H, _Outptr_result_maybenull_ PUCHAR *Pcm, _Out_ ULONG *PcmCapacity)
{
    *Pcm = NULL;
    *PcmCapacity = 0;
    /* Once removed from the cancel-safe queue the IRP can no longer be cancelled or
     * flushed: the caller owns it until CodecHostCompleteWork. */
    PIRP irp = IoCsqRemoveNextIrp(&H->WorkCsq, NULL);
    if (!irp) return NULL;
    ULONG cap = IoGetCurrentIrpStackLocation(irp)->Parameters.DeviceIoControl.OutputBufferLength;
    if (cap > sizeof(PNA2DP_CODEC_WORK)) {
        *Pcm = (PUCHAR)((PNA2DP_CODEC_WORK *)irp->AssociatedIrp.SystemBuffer + 1);
        *PcmCapacity = cap - sizeof(PNA2DP_CODEC_WORK);
    }
    return irp;
}

VOID CodecHostCompleteWork(_Inout_ PPN_CODEC_HOST H, _In_ PIRP Irp, _In_ const PNA2DP_CODEC_WORK *Header, _In_ ULONG PcmBytes)
{
    UNREFERENCED_PARAMETER(H);
    ULONG cap = IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength;
    if (cap < sizeof(PNA2DP_CODEC_WORK) || PcmBytes > cap - sizeof(PNA2DP_CODEC_WORK)) {
        Irp->IoStatus.Status = STATUS_BUFFER_TOO_SMALL;
        Irp->IoStatus.Information = 0;
        IoCompleteRequest(Irp, IO_NO_INCREMENT);
        return;
    }
    PNA2DP_CODEC_WORK *out = (PNA2DP_CODEC_WORK *)Irp->AssociatedIrp.SystemBuffer;
    *out = *Header;   /* the PCM after the header is already in place */
    out->Size = sizeof(PNA2DP_CODEC_WORK) + PcmBytes;
    out->PcmBytes = PcmBytes;
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = out->Size;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}
