/*
 * device.cpp - per-device orchestration: connection state machine, codec selection,
 * audio <-> AVDTP reconciliation, SCO handling, events for user mode.
 *
 * Locking order: StateMutex -> Avdtp.Mutex -> spin locks. AVDTP callbacks run with
 * Avdtp.Mutex held and must therefore never take StateMutex; they record state and
 * queue work items.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

static LIST_ENTRY g_DeviceList;
static KSPIN_LOCK g_DeviceListLock;

/* Called once from DriverEntry, before any device can be added. */
VOID DeviceListInitialize(void)
{
    InitializeListHead(&g_DeviceList);
    KeInitializeSpinLock(&g_DeviceListLock);
}

VOID DeviceReference(_Inout_ PPNA2DP_DEVICE D)
{
    InterlockedIncrement(&D->RefCount);
}

VOID DeviceRelease(_In_ PPNA2DP_DEVICE D)
{
    if (InterlockedDecrement(&D->RefCount) != 0) return;
    /* Last reference: every user is gone. Streams may have armed the suspend timer after
     * DeviceStop, so cancel the timers once more before the memory goes away. */
    KeCancelTimer(&D->SuspendTimer);
    KeCancelTimer(&D->ReconnectTimer);
    KeFlushQueuedDpcs();
    D->Signature = 0;
    PnFree(D, PNA2DP_TAG_DEVICE);
}

PPNA2DP_DEVICE DeviceReferenceFromFdo(_In_ PDEVICE_OBJECT Fdo)
{
    PPNA2DP_DEVICE found = NULL;
    KIRQL irql;
    KeAcquireSpinLock(&g_DeviceListLock, &irql);
    for (PLIST_ENTRY e = g_DeviceList.Flink; e != &g_DeviceList; e = e->Flink) {
        PPNA2DP_DEVICE d = CONTAINING_RECORD(e, PNA2DP_DEVICE, GlobalLink);
        if (d->Fdo == Fdo && !d->Removing) { DeviceReference(d); found = d; break; }
    }
    KeReleaseSpinLock(&g_DeviceListLock, irql);
    return found;
}

/* Work items hold rundown protection from queueing until the end of the worker, so
 * DeviceStop can wait for them before anything they touch is freed. */
static BOOLEAN DeviceQueueWorkItem(_Inout_ PPNA2DP_DEVICE D, _In_ PIO_WORKITEM Item, _In_ PIO_WORKITEM_ROUTINE Routine)
{
    if (D->Removing || !Item || !ExAcquireRundownProtection(&D->WorkRundown)) return FALSE;
    IoQueueWorkItem(Item, Routine, DelayedWorkQueue, D);
    return TRUE;
}

/* ------------------------------------------------------------------------- */
/* Event waiters (IOCTL_PNA2DP_WAIT_EVENT)                                      */
/* ------------------------------------------------------------------------- */

static PN_EVENT_WAITER *WaiterFromCsq(PIO_CSQ Csq) { return CONTAINING_RECORD(Csq, PN_EVENT_WAITER, Csq); }
static VOID WCsqInsert(_In_ PIO_CSQ Csq, _In_ PIRP Irp) { InsertTailList(&WaiterFromCsq(Csq)->Queue, &Irp->Tail.Overlay.ListEntry); }
static VOID WCsqRemove(_In_ PIO_CSQ Csq, _In_ PIRP Irp) { UNREFERENCED_PARAMETER(Csq); RemoveEntryList(&Irp->Tail.Overlay.ListEntry); }
static PIRP WCsqPeek(_In_ PIO_CSQ Csq, _In_opt_ PIRP Irp, _In_opt_ PVOID Ctx)
{
    PN_EVENT_WAITER *w = WaiterFromCsq(Csq);
    PLIST_ENTRY e = Irp ? Irp->Tail.Overlay.ListEntry.Flink : w->Queue.Flink;
    while (e != &w->Queue) {
        PIRP irp = CONTAINING_RECORD(e, IRP, Tail.Overlay.ListEntry);
        if (!Ctx || IoGetCurrentIrpStackLocation(irp)->FileObject == (PFILE_OBJECT)Ctx) return irp;
        e = e->Flink;
    }
    return NULL;
}
_IRQL_raises_(DISPATCH_LEVEL) _IRQL_requires_max_(DISPATCH_LEVEL)
static VOID WCsqAcquire(_In_ PIO_CSQ Csq, _Out_ PKIRQL Irql) { KeAcquireSpinLock(&WaiterFromCsq(Csq)->Lock, Irql); }
_IRQL_requires_(DISPATCH_LEVEL)
static VOID WCsqRelease(_In_ PIO_CSQ Csq, _In_ KIRQL Irql) { KeReleaseSpinLock(&WaiterFromCsq(Csq)->Lock, Irql); }
static VOID WCsqCancel(_In_ PIO_CSQ Csq, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Csq);
    Irp->IoStatus.Status = STATUS_CANCELLED;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
}

NTSTATUS DeviceQueueWaitEvent(_Inout_ PPNA2DP_DEVICE D, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    if (sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(ULONG)) return STATUS_BUFFER_TOO_SMALL;
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    ULONG pending = D->PendingEvents;
    D->PendingEvents = 0;
    // Insertion and the notifier's no-waiter decision share this lock.
    if (!pending) IoCsqInsertIrp(&D->Waiters.Csq, Irp, NULL);
    KeReleaseSpinLock(&D->StatusLock, irql);
    if (pending) {
        *(PULONG)Irp->AssociatedIrp.SystemBuffer = pending;
        Irp->IoStatus.Information = sizeof(ULONG);
        return STATUS_SUCCESS;
    }
    return STATUS_PENDING;
}

VOID DeviceNotifyEvent(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG EventMask)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    EventMask |= D->PendingEvents;
    PIRP irp = IoCsqRemoveNextIrp(&D->Waiters.Csq, NULL);
    D->PendingEvents = irp ? 0 : EventMask;
    KeReleaseSpinLock(&D->StatusLock, irql);
    while (irp) {
        *(PULONG)irp->AssociatedIrp.SystemBuffer = EventMask;
        irp->IoStatus.Status = STATUS_SUCCESS;
        irp->IoStatus.Information = sizeof(ULONG);
        IoCompleteRequest(irp, IO_NO_INCREMENT);
        KeAcquireSpinLock(&D->StatusLock, &irql);
        irp = IoCsqRemoveNextIrp(&D->Waiters.Csq, NULL);
        KeReleaseSpinLock(&D->StatusLock, irql);
    }
}

VOID DeviceFlushWaitersForFile(_Inout_ PPNA2DP_DEVICE D, _In_ PFILE_OBJECT File)
{
    PIRP irp;
    while ((irp = IoCsqRemoveNextIrp(&D->Waiters.Csq, File)) != NULL) {
        irp->IoStatus.Status = STATUS_CANCELLED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
}

/* ------------------------------------------------------------------------- */
/* Status helpers                                                               */
/* ------------------------------------------------------------------------- */

static VOID UpdateStatusLocked(_Inout_ PPNA2DP_DEVICE D)
{
    PNA2DP_STATUS *s = &D->Status;
    s->Size = sizeof(*s);
    s->RemoteAddress = D->Link.RemoteAddress;
    switch (AvdtpGetState(&D->Avdtp)) {
    case AvdtpStateIdle: s->State = D->Avdtp.SignallingUp ? PNA2DP_STATE_DISCOVERING : PNA2DP_STATE_IDLE; break;
    case AvdtpStateConfigured: s->State = PNA2DP_STATE_CONFIGURED; break;
    case AvdtpStateOpening: s->State = PNA2DP_STATE_CONFIGURED; break;
    case AvdtpStateOpen: s->State = PumpIsStreaming(&D->Pump) ? PNA2DP_STATE_STREAMING : PNA2DP_STATE_OPEN; break;
    case AvdtpStateStreaming: s->State = PNA2DP_STATE_STREAMING; break;
    case AvdtpStateClosing:
    case AvdtpStateAborting: s->State = PNA2DP_STATE_CLOSING; break;
    }
    if (D->ActiveValid) {
        s->Codec = D->Active.Codec;
        s->SampleRate = D->Active.SampleRate;
        s->Channels = D->Active.Channels;
        s->BitsPerSample = (D->Active.PcmFormat == PNA2DP_FMT_S16) ? 16 : (D->Active.PcmFormat == PNA2DP_FMT_S24) ? 24 : 32;
        s->BitrateBps = PumpIsStreaming(&D->Pump) ? PumpCurrentBitrate(&D->Pump) : D->Active.BitrateBps;
        s->MaxBitrateBps = D->Active.MaxBitrateBps;
        s->RemoteSeid = D->Avdtp.RemoteSeid;
    } else {
        s->Codec = PNA2DP_CODEC_NONE;
        s->SampleRate = s->Channels = s->BitsPerSample = s->BitrateBps = s->MaxBitrateBps = 0;
    }
    s->MediaMtu = AvdtpMediaMtu(&D->Avdtp);
    s->SinkReportedDelayUs = D->Avdtp.SinkDelayTenthsMs * 100;
    s->EstimatedLatencyUs = PumpEstimatedLatencyUs(&D->Pump) + s->SinkReportedDelayUs;
    s->AvdtpVersion = D->Avdtp.RemoteVersion;
    s->EncoderOwner = (ULONG)D->Pump.Owner;
    s->Flags = 0;
    if (D->Link.ScoActive) s->Flags |= PNA2DP_STATUS_FLAG_SCO_ACTIVE;
    if (D->Pump.Abr.Enabled && D->Pump.Abr.Level) s->Flags |= PNA2DP_STATUS_FLAG_ABR_ACTIVE;
    if (D->Avdtp.DelayReportingNegotiated) s->Flags |= PNA2DP_STATUS_FLAG_DELAY_REPORTING;
    if (D->Avdtp.RemoteSignallingInitiated) s->Flags |= PNA2DP_STATUS_FLAG_REMOTE_INITIATED;
    if (D->AudioRequest == PnAudioRun) s->Flags |= PNA2DP_STATUS_FLAG_AUDIO_ACTIVE;
    if (D->Link.AclConnected) s->Flags |= PNA2DP_STATUS_FLAG_RADIO_IN_RANGE;
    if (D->RemoteVolumeActive) s->Flags |= PNA2DP_STATUS_FLAG_REMOTE_VOLUME;
    if (D->Pump.Owner == PnEncoderKernelSbc && D->Config.CodecPriority[0] != PNA2DP_CODEC_SBC && !CodecHostCodecMask(&D->Host)) s->Flags |= PNA2DP_STATUS_FLAG_HOST_MISSING;
}

NTSTATUS DeviceGetStatus(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_STATUS Status)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    UpdateStatusLocked(D);
    *Status = D->Status;
    KeReleaseSpinLock(&D->StatusLock, irql);
    RtlStringCchCopyW(Status->RemoteName, PN_ARRAYSIZE(Status->RemoteName), D->Link.RemoteNameW);
    return STATUS_SUCCESS;
}

NTSTATUS DeviceGetCapabilities(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CAPABILITIES Caps)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    *Caps = D->Caps;
    KeReleaseSpinLock(&D->StatusLock, irql);
    Caps->Size = sizeof(*Caps);
    return STATUS_SUCCESS;
}

NTSTATUS DeviceGetConfig(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CONFIG Cfg)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    *Cfg = D->Config;
    KeReleaseSpinLock(&D->StatusLock, irql);
    return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------- */
/* Codec selection                                                              */
/* ------------------------------------------------------------------------- */

static VOID RefreshCapabilities(_Inout_ PPNA2DP_DEVICE D)
{
    PNA2DP_CAPABILITIES caps;
    RtlZeroMemory(&caps, sizeof(caps));
    caps.Size = sizeof(caps);
    caps.SdpProfileVersion = D->Link.RemoteAvdtpVersion;
    caps.SdpSupportedFeatures = D->Link.RemoteSupportedFeatures;
    for (ULONG i = 0; i < D->Avdtp.RemoteSepCount && caps.SepCount < PNA2DP_MAX_REMOTE_SEPS; i++) {
        AVDTP_REMOTE_SEP *r = &D->Avdtp.RemoteSeps[i];
        if (!r->CapsValid) continue;
        PNA2DP_REMOTE_SEP *o = &caps.Seps[caps.SepCount++];
        o->Seid = r->Seid;
        o->InUse = r->InUse;
        o->MediaType = r->MediaType;
        o->TsepIsSink = r->Tsep;
        o->Codec = r->Codec;
        o->VendorId = r->VendorId;
        o->VendorCodecId = r->VendorCodecId;
        o->CodecInfoLength = r->CodecCapsLength;
        RtlCopyMemory(o->CodecInfo, r->CodecCaps, r->CodecCapsLength);
        o->ServiceCaps = r->ServiceCaps;
        if (r->Codec) caps.CodecMask |= PNA2DP_CODEC_BIT(r->Codec);
    }
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Caps = caps;
    KeReleaseSpinLock(&D->StatusLock, irql);
    RegCfgSaveCapabilities(D->DriverObject, D->Link.RemoteAddress, &caps);   /* PASSIVE: StateMutex is a PnMutex */
    DeviceNotifyEvent(D, PNA2DP_EVENT_CAPS_CHANGED);
}

static BOOLEAN EncoderAvailable(_In_ PPNA2DP_DEVICE D, _In_ ULONG Codec, _Out_ PN_ENCODER_OWNER *Owner)
{
    if (Codec == PNA2DP_CODEC_SBC && !(D->Config.Flags & PNA2DP_CFG_HOST_CODECS_ONLY)) { *Owner = PnEncoderKernelSbc; return TRUE; }
    if (CodecHostAvailable(&D->Host, Codec)) { *Owner = PnEncoderHost; return TRUE; }
    if (Codec == PNA2DP_CODEC_SBC) { *Owner = PnEncoderKernelSbc; return TRUE; }
    return FALSE;
}

static PAVDTP_LOCAL_SEP LocalSepForCodec(_In_ PPNA2DP_DEVICE D, _In_ ULONG Codec)
{
    for (ULONG i = 0; i < D->Avdtp.LocalSepCount; i++) if (D->Avdtp.LocalSeps[i].Codec == Codec) return &D->Avdtp.LocalSeps[i];
    return NULL;
}

/* Picks codec/config according to priority list. Returns the remote SEP and fills Cfg. */
static AVDTP_REMOTE_SEP *ChooseConfig(_Inout_ PPNA2DP_DEVICE D, _Out_ PA2DP_CODEC_CONFIG Cfg, _Out_ PN_ENCODER_OWNER *Owner, _Out_ PUCHAR LocalSeid)
{
    BOOLEAN conservative = (D->Config.Flags & PNA2DP_CFG_AUTO_QUALITY) &&
        PnAdapterNeedsConservativeProfile(D->Link.RadioInfoValid, D->Link.RadioInfo.radioInfo.lmpSupportedFeatures);
    ULONG prefRate = D->AudioStreamOpen ? D->AudioRate : 48000;
    ULONG prefBits = D->AudioStreamOpen ? D->AudioValidBits : 16;
    for (ULONG pi = 0; pi < PN_ARRAYSIZE(D->Config.CodecPriority); pi++) {
        ULONG codec = D->Config.CodecPriority[pi];
        if (!codec) break;
        if (!(D->Config.EnabledCodecMask & PNA2DP_CODEC_BIT(codec))) continue;
        PN_ENCODER_OWNER owner;
        if (!EncoderAvailable(D, codec, &owner)) { PN_VERB("codec %s: no encoder available", A2dpCodecName(codec)); continue; }
        PAVDTP_LOCAL_SEP local = LocalSepForCodec(D, codec);
        if (!local) continue;
        for (ULONG i = 0; i < D->Avdtp.RemoteSepCount; i++) {
            AVDTP_REMOTE_SEP *r = &D->Avdtp.RemoteSeps[i];
            if (!r->CapsValid || r->Codec != codec || r->Tsep != AVDTP_TSEP_SINK || r->InUse) continue;
            A2DP_CODEC_CAPS caps;
            if (!A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL)) continue;
            if (D->AudioStreamOpen) caps.RateMask &= A2dpHzToRateMask(prefRate);
            if (A2dpSelectConfig(&caps, &D->Config, prefRate, prefBits, Cfg)) {
                if (conservative && !A2dpConstrainBasicRadio(Cfg)) continue;
                *Owner = owner;
                *LocalSeid = local->Seid;
                return r;
            }
        }
    }
    /* last resort: SBC with any sink SEP */
    for (ULONG i = 0; i < D->Avdtp.RemoteSepCount; i++) {
        AVDTP_REMOTE_SEP *r = &D->Avdtp.RemoteSeps[i];
        if (!r->CapsValid || r->Codec != PNA2DP_CODEC_SBC || r->Tsep != AVDTP_TSEP_SINK || r->InUse) continue;
        A2DP_CODEC_CAPS caps;
        if (!A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL)) continue;
        if (D->AudioStreamOpen) caps.RateMask &= A2dpHzToRateMask(prefRate);
        if (A2dpSelectConfig(&caps, &D->Config, prefRate, prefBits, Cfg)) {
            if (conservative && !A2dpConstrainBasicRadio(Cfg)) continue;
            PAVDTP_LOCAL_SEP local = LocalSepForCodec(D, PNA2DP_CODEC_SBC);
            *Owner = PnEncoderKernelSbc;
            *LocalSeid = local ? local->Seid : 1;
            return r;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Reconcile: make reality match AudioRequest (PASSIVE, StateMutex held)        */
/* ------------------------------------------------------------------------- */

static VOID ScheduleReconnect(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG DelayMs)
{
    if (D->Removing) return;   /* DeviceStop cancels the timer after every source is stopped */
    LARGE_INTEGER due;
    due.QuadPart = -PN_HNS_PER_MS * (LONGLONG)DelayMs;
    KeSetTimer(&D->ReconnectTimer, due, &D->ReconnectDpc);
}

static NTSTATUS EnsureSignalling(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Avdtp.SignallingUp) return STATUS_SUCCESS;
    NTSTATUS status = AvdtpConnectSignalling(&D->Avdtp);
    if (!NT_SUCCESS(status)) {
        D->Status.LastError = (ULONG)status;
        return status;
    }
    /* SDP is best effort: learn the AVDTP version so GET_ALL_CAPABILITIES is used correctly */
    if (D->Link.RemoteAvdtpVersion == 0) {
        if (NT_SUCCESS(PnLinkQueryRemoteSdp(&D->Link, A2DP_UUID_AUDIO_SINK)) && D->Link.RemoteAvdtpVersion) {
            D->Avdtp.RemoteVersion = D->Link.RemoteAvdtpVersion;
        }
    }
    D->ReconnectAttempts = 0;
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    return STATUS_SUCCESS;
}

static NTSTATUS EnsureDiscovered(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Avdtp.RemoteSepsValid) return STATUS_SUCCESS;
    NTSTATUS status = AvdtpDiscover(&D->Avdtp);
    if (NT_SUCCESS(status)) RefreshCapabilities(D);
    else D->Status.LastError = (ULONG)status;
    return status;
}

static NTSTATUS EnsureConfiguredAndOpen(_Inout_ PPNA2DP_DEVICE D)
{
    AVDTP_STREAM_STATE st = AvdtpGetState(&D->Avdtp);
    if (st == AvdtpStateIdle) {
        A2DP_CODEC_CONFIG cfg;
        PN_ENCODER_OWNER owner;
        UCHAR localSeid;
        AVDTP_REMOTE_SEP *r = ChooseConfig(D, &cfg, &owner, &localSeid);
        if (!r) { PN_ERR("no usable codec/SEP on the sink"); D->Status.LastError = (ULONG)STATUS_NOT_SUPPORTED; return STATUS_NOT_SUPPORTED; }
        BOOLEAN delay = (D->Config.Flags & PNA2DP_CFG_DELAY_REPORTING) && (r->ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) && D->Avdtp.RemoteVersion >= 0x0103;
        PN_INFO("configuring %s @ %u Hz, chmode %u, local seid %u -> remote seid %u (owner %u)", A2dpCodecName(cfg.Codec), cfg.SampleRate, cfg.ChannelMode, localSeid, r->Seid, owner);
        NTSTATUS status = AvdtpSetConfiguration(&D->Avdtp, localSeid, r->Seid, cfg.Ies, cfg.IesLength, delay, FALSE);
        if (!NT_SUCCESS(status) && delay) {
            PN_WARN("retrying SET_CONFIGURATION without delay reporting");
            status = AvdtpSetConfiguration(&D->Avdtp, localSeid, r->Seid, cfg.Ies, cfg.IesLength, FALSE, FALSE);
        }
        if (!NT_SUCCESS(status)) { D->Status.LastError = (ULONG)status; return status; }
        KIRQL irql;
        KeAcquireSpinLock(&D->StatusLock, &irql);
        D->Active = cfg;
        D->ActiveValid = TRUE;
        KeReleaseSpinLock(&D->StatusLock, irql);
        st = AvdtpStateConfigured;   /* StartPumpIfNeeded picks the encoder owner again */
    }
    if (st == AvdtpStateConfigured) {
        NTSTATUS status = AvdtpOpen(&D->Avdtp);
        if (!NT_SUCCESS(status)) {
            D->Status.LastError = (ULONG)status;
            if (status == STATUS_DEVICE_PROTOCOL_ERROR || status == STATUS_NOT_SUPPORTED) {
                /* the sink no longer has this stream configured: abort to Idle so the next
                 * attempt sends SET_CONFIGURATION again instead of repeating OPEN */
                AvdtpAbort(&D->Avdtp);
                KIRQL irql;
                KeAcquireSpinLock(&D->StatusLock, &irql);
                D->ActiveValid = FALSE;
                KeReleaseSpinLock(&D->StatusLock, irql);
            }
            return status;
        }
        st = AvdtpStateOpen;
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    }
    if (st == AvdtpStateOpening) return STATUS_PENDING;   /* remote is opening the transport */
    return STATUS_SUCCESS;
}

static NTSTATUS StartPumpIfNeeded(_Inout_ PPNA2DP_DEVICE D)
{
    if (PumpIsStreaming(&D->Pump)) return STATUS_SUCCESS;
    if (!D->ActiveValid || !D->AudioStreamOpen) return STATUS_INVALID_DEVICE_STATE;
    PN_ENCODER_OWNER owner;
    if (!EncoderAvailable(D, D->Active.Codec, &owner)) {
        PN_WARN("encoder for %s vanished; closing stream to renegotiate", A2dpCodecName(D->Active.Codec));
        AvdtpClose(&D->Avdtp);
        D->ActiveValid = FALSE;
        return STATUS_DEVICE_NOT_READY;
    }
    NTSTATUS status = PumpStartStream(&D->Pump, &D->Active, AvdtpMediaChannel(&D->Avdtp), AvdtpMediaMtu(&D->Avdtp), owner, &D->Config);
    if (!NT_SUCCESS(status)) D->Status.LastError = (ULONG)status;
    PumpSetVolume(&D->Pump, D->RemoteVolumeActive ? 0 : D->VolumeDb16, D->Muted);
    return status;
}

static VOID Reconcile(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN SuspendDue)
{
    PAGED_CODE();
    if (D->Removing) return;
    BOOLEAN scoBlock = D->Link.ScoActive && (D->Config.Flags & PNA2DP_CFG_SUSPEND_ON_SCO);
    BOOLEAN want = (D->AudioRequest == PnAudioRun) && D->AudioStreamOpen && !scoBlock;
    AVDTP_STREAM_STATE st = AvdtpGetState(&D->Avdtp);

    if (want) {
        /* the remote closed or aborted the stream under a running pump: stop it before
         * anything is reconfigured, so the next PumpStartStream sets the encoder up again */
        if (PumpIsStreaming(&D->Pump) && st != AvdtpStateStreaming && st != AvdtpStateOpen) PumpStopStream(&D->Pump);
        /* the codec host went away while it was encoding: renegotiate (SBC in the kernel) */
        if (PumpIsStreaming(&D->Pump) && D->Pump.Owner == PnEncoderHost && !CodecHostCodecMask(&D->Host)) {
            PN_WARN("codec host lost while streaming %s; renegotiating", A2dpCodecName(D->Active.Codec));
            PumpStopStream(&D->Pump);
            AvdtpClose(&D->Avdtp);
            D->ActiveValid = FALSE;
        }
        if (!NT_SUCCESS(EnsureSignalling(D))) {
            if (D->ReconnectAttempts < 10) { D->ReconnectAttempts++; ScheduleReconnect(D, D->Config.ReconnectDelayMs + 500 * D->ReconnectAttempts); }
            DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR);
            return;
        }
        if (!NT_SUCCESS(EnsureDiscovered(D))) { DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR); return; }
        /* sample-rate mismatch between engine and negotiated config: reconfigure when the sink allows it */
        st = AvdtpGetState(&D->Avdtp);
        if (D->ActiveValid && D->AudioStreamOpen && D->Active.SampleRate != D->AudioRate && (st == AvdtpStateOpen || st == AvdtpStateStreaming || st == AvdtpStateConfigured)) {
            A2DP_CODEC_CONFIG cfg = D->Active;
            AVDTP_REMOTE_SEP *r = NULL;
            for (ULONG i = 0; i < D->Avdtp.RemoteSepCount; i++) if (D->Avdtp.RemoteSeps[i].Seid == D->Avdtp.RemoteSeid) r = &D->Avdtp.RemoteSeps[i];
            A2DP_CODEC_CAPS caps;
            if (r && A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL) && (caps.RateMask & A2dpHzToRateMask(D->AudioRate))) {
                cfg.SampleRate = D->AudioRate;
                A2dpSerializeConfig(&cfg);
                PN_INFO("reconfiguring stream to %u Hz for the audio engine", D->AudioRate);
                if (PumpIsStreaming(&D->Pump)) PumpStopStream(&D->Pump);
                if (st == AvdtpStateStreaming) AvdtpSuspend(&D->Avdtp);
                NTSTATUS rs = (AvdtpGetState(&D->Avdtp) == AvdtpStateOpen) ? AvdtpReconfigure(&D->Avdtp, cfg.Ies, cfg.IesLength) : STATUS_INVALID_DEVICE_STATE;
                if (!NT_SUCCESS(rs)) {
                    AvdtpClose(&D->Avdtp);
                    D->ActiveValid = FALSE;
                } else {
                    cfg.BitrateBps = cfg.MaxBitrateBps = A2dpNominalBitrate(&cfg);
                    D->Active = cfg;
                }
            } else {
                PN_WARN("sink does not support %u Hz for %s; selecting another codec", D->AudioRate, A2dpCodecName(cfg.Codec));
                PumpStopStream(&D->Pump);
                AvdtpClose(&D->Avdtp);
                D->ActiveValid = FALSE;
            }
        }
        NTSTATUS status = EnsureConfiguredAndOpen(D);
        if (status == STATUS_PENDING) return;
        if (!NT_SUCCESS(status)) {
            DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR);
            if (status == STATUS_IO_TIMEOUT || status == STATUS_CONNECTION_DISCONNECTED) { AvdtpDisconnect(&D->Avdtp); ScheduleReconnect(D, 1500); }
            return;
        }
        st = AvdtpGetState(&D->Avdtp);
        if (st == AvdtpStateOpen || st == AvdtpStateStreaming) {
            if (st == AvdtpStateOpen) {
                /* A media channel may stay open while its ACL goes into sniff mode.
                 * Wake the link before the latency-sensitive START transaction. */
                PnLinkEnsureActive(&D->Link);
                status = AvdtpStart(&D->Avdtp);
                if (!NT_SUCCESS(status)) { DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR); return; }
            }
            if (!NT_SUCCESS(StartPumpIfNeeded(D))) {
                if (AvdtpGetState(&D->Avdtp) == AvdtpStateStreaming) AvdtpSuspend(&D->Avdtp);
                DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR);
                return;
            }
            DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
        }
    } else {
        if (PumpIsStreaming(&D->Pump) && (st != AvdtpStateStreaming || SuspendDue || scoBlock || !D->AudioStreamOpen)) {
            PumpStopStream(&D->Pump);
        }
        if (st == AvdtpStateStreaming && (SuspendDue || scoBlock || !D->AudioStreamOpen)) {
            AvdtpSuspend(&D->Avdtp);
            DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Work items / timers                                                          */
/* ------------------------------------------------------------------------- */

static VOID ConnectWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (!D) return;
    InterlockedExchange(&D->ConnectQueued, 0);
    PnMutexAcquire(&D->StateMutex);
    if (!D->Removing) {
        /* Opportunistic connect: bring the signalling channel up and learn capabilities so
         * the endpoint's format list matches the sink before audio starts. When audio is
         * already wanted, Reconcile does the same with retry scheduling; connecting here too
         * would double the time a speaker that is off holds StateMutex. */
        BOOLEAN want = D->AudioRequest == PnAudioRun && D->AudioStreamOpen;
        if (!want && NT_SUCCESS(EnsureSignalling(D))) {
            EnsureDiscovered(D);
            if (D->Config.Flags & PNA2DP_CFG_AUTO_START_ON_CONNECT) EnsureConfiguredAndOpen(D);
        }
        Reconcile(D, FALSE);
    }
    PnMutexRelease(&D->StateMutex);
    ExReleaseRundownProtection(&D->WorkRundown);   /* last access to D */
}

VOID DeviceQueueConnect(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Removing) return;
    if (InterlockedCompareExchange(&D->ConnectQueued, 1, 0) == 0) {
        if (!DeviceQueueWorkItem(D, D->ConnectWorkItem, ConnectWorker)) InterlockedExchange(&D->ConnectQueued, 0);
    }
}

static VOID AudioWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (!D) return;
    LONG flags = InterlockedExchange(&D->AudioQueued, 0);
    PnMutexAcquire(&D->StateMutex);
    Reconcile(D, (flags & 2) != 0);
    PnMutexRelease(&D->StateMutex);
    ExReleaseRundownProtection(&D->WorkRundown);
}

static VOID QueueAudioWork(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN SuspendDue)
{
    if (D->Removing) return;
    LONG old, next;
    do { old = D->AudioQueued; next = old | 1 | (SuspendDue ? 2 : 0); } while (InterlockedCompareExchange(&D->AudioQueued, next, old) != old);
    if (old == 0 && !DeviceQueueWorkItem(D, D->AudioWorkItem, AudioWorker)) InterlockedExchange(&D->AudioQueued, 0);
}

static VOID SuspendDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (D && D->AudioRequest != PnAudioRun) QueueAudioWork(D, TRUE);
}

static VOID ReconnectDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (D) DeviceQueueConnect(D);
}

static VOID ScoWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (!D) return;
    InterlockedExchange(&D->ScoQueued, 0);
    PnMutexAcquire(&D->StateMutex);
    Reconcile(D, FALSE);
    PnMutexRelease(&D->StateMutex);
    DeviceNotifyEvent(D, PNA2DP_EVENT_SCO_CHANGED);
    ExReleaseRundownProtection(&D->WorkRundown);
}

/* ------------------------------------------------------------------------- */
/* Link callbacks                                                               */
/* ------------------------------------------------------------------------- */

static VOID OnIncoming(_In_ PVOID Context, _In_ PINDICATION_PARAMETERS Params)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (D->Removing) { PnLinkRejectIncoming(&D->Link, Params); return; }
    NTSTATUS status = AvdtpAcceptIncoming(&D->Avdtp, Params);
    if (!NT_SUCCESS(status)) PN_WARN("accept incoming failed 0x%08x", status);
}

static VOID OnSco(_In_ PVOID Context, _In_ BOOLEAN Active)
{
    UNREFERENCED_PARAMETER(Active);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (D->Removing) return;
    if (InterlockedCompareExchange(&D->ScoQueued, 1, 0) == 0) {
        if (!DeviceQueueWorkItem(D, D->ScoWorkItem, ScoWorker)) InterlockedExchange(&D->ScoQueued, 0);
    }
}

static VOID OnAcl(_In_ PVOID Context, _In_ BOOLEAN Connected)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (D->Removing) return;
    if (Connected) {
        /* give the headset a chance to initiate AVDTP itself before we do */
        if (!D->Avdtp.SignallingUp) ScheduleReconnect(D, 2500);
    }
}

/* ------------------------------------------------------------------------- */
/* AVDTP callbacks (Avdtp.Mutex held, PASSIVE)                                  */
/* ------------------------------------------------------------------------- */

static UCHAR OnRemoteSetConfiguration(_In_ PVOID Ctx, _In_ PAVDTP_LOCAL_SEP Local, _In_ UCHAR IntSeid, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                                      _In_ ULONG ServiceCaps, _Out_ PUCHAR FailedCategory)
{
    UNREFERENCED_PARAMETER(IntSeid);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    *FailedCategory = AVDTP_CAT_MEDIA_CODEC;
    if (!(ServiceCaps & PNA2DP_SVC_MEDIA_TRANSPORT)) { *FailedCategory = AVDTP_CAT_MEDIA_TRANSPORT; return AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT; }
    UCHAR mct; UCHAR dummy[32];
    A2dpBuildLocalCaps(Local->Codec, dummy, sizeof(dummy), &mct);
    A2DP_CODEC_CONFIG cfg;
    if (!A2dpParseConfig(mct, Ies, Len, &cfg) || cfg.Codec != Local->Codec) return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
    if (!(D->Config.EnabledCodecMask & PNA2DP_CODEC_BIT(cfg.Codec))) return A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE;
    PN_ENCODER_OWNER owner;
    if (!EncoderAvailable(D, cfg.Codec, &owner)) return A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE;
    if (cfg.Codec == PNA2DP_CODEC_SBC) {
        if (cfg.SbcMaxBitpool > 250 || cfg.SbcMinBitpool < 2) return A2DP_ERR_INVALID_MAX_BITPOOL;
        if (cfg.SbcMinBitpool > cfg.SbcMaxBitpool) return A2DP_ERR_INVALID_MIN_BITPOOL;
    }
    /* only one sample rate / channel mode allowed in a configuration: ParseConfig picked one */
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Active = cfg;
    D->ActiveValid = TRUE;
    KeReleaseSpinLock(&D->StatusLock, irql);
    PN_INFO("remote configured %s @ %u Hz (encoder %u)", A2dpCodecName(cfg.Codec), cfg.SampleRate, owner);
    return 0;
}

static UCHAR OnRemoteReconfigure(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len, _Out_ PUCHAR FailedCategory)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    *FailedCategory = AVDTP_CAT_MEDIA_CODEC;
    if (!D->ActiveValid) return AVDTP_ERR_BAD_STATE;
    A2DP_CODEC_CONFIG cfg;
    if (!A2dpParseConfig(D->Active.MediaCodecType, Ies, Len, &cfg) || cfg.Codec != D->Active.Codec) return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Active = cfg;
    KeReleaseSpinLock(&D->StatusLock, irql);
    QueueAudioWork(D, FALSE);
    return 0;
}

static VOID OnStreamStateChanged(_In_ PVOID Ctx, _In_ AVDTP_STREAM_STATE Old, _In_ AVDTP_STREAM_STATE New, _In_ BOOLEAN Remote)
{
    UNREFERENCED_PARAMETER(Old);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (New == AvdtpStateIdle) {
        KIRQL irql;
        KeAcquireSpinLock(&D->StatusLock, &irql);
        D->ActiveValid = FALSE;
        KeReleaseSpinLock(&D->StatusLock, irql);
    }
    if (Remote) QueueAudioWork(D, FALSE);   /* reconcile pump with the remote's decision */
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
}

static VOID OnDelayReport(_In_ PVOID Ctx, _In_ ULONG DelayTenthsMs)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    PN_INFO("sink delay report %u.%u ms", DelayTenthsMs / 10, DelayTenthsMs % 10);
    DeviceNotifyEvent(D, PNA2DP_EVENT_DELAY_REPORT);
}

static VOID OnSignallingDown(_In_ PVOID Ctx, _In_ NTSTATUS Status, _In_ ULONG Generation)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    UNREFERENCED_PARAMETER(Status);
    if (D->Removing) return;
    PnMutexAcquire(&D->StateMutex);
    /* every outgoing connect runs under StateMutex: if one replaced the channel while this
     * waited, the event is about a connection that no longer exists */
    if (Generation != D->Avdtp.Signalling.Generation) { PnMutexRelease(&D->StateMutex); return; }
    PumpStopStream(&D->Pump);
    AvdtpDisconnect(&D->Avdtp);
    D->ActiveValid = FALSE;
    PnMutexRelease(&D->StateMutex);
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    if (D->AudioRequest == PnAudioRun && D->AudioStreamOpen) ScheduleReconnect(D, D->Config.ReconnectDelayMs);
}

static VOID OnMediaPacket(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(Ctx); UNREFERENCED_PARAMETER(Data); UNREFERENCED_PARAMETER(Len);
    /* sink role (phase 2): decode and feed a capture pin */
}

static VOID OnMediaDown(_In_ PVOID Ctx, _In_ NTSTATUS Status, _In_ ULONG Generation)
{
    UNREFERENCED_PARAMETER(Status);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (D->Removing) return;
    PnMutexAcquire(&D->StateMutex);
    if (Generation != D->Avdtp.Media.Generation) { PnMutexRelease(&D->StateMutex); return; }
    PumpStopStream(&D->Pump);
    PnMutexRelease(&D->StateMutex);
    QueueAudioWork(D, FALSE);
}

VOID DeviceMediaWriteDone(PVOID Ctx, NTSTATUS Status, ULONG Bytes, LONGLONG SubmitTimeHns)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    PumpOnWriteDone(&D->Pump, Status, Bytes, SubmitTimeHns);
}

/* ------------------------------------------------------------------------- */
/* Audio endpoint -> device                                                     */
/* ------------------------------------------------------------------------- */

NTSTATUS DeviceAudioStreamOpened(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat)
{
    PAGED_CODE();
    PnMutexAcquire(&D->StateMutex);
    if (D->Removing || D->TornDown) { PnMutexRelease(&D->StateMutex); return STATUS_DEVICE_REMOVED; }
    NTSTATUS status = PumpConfigureInput(&D->Pump, Rate, Channels, ContainerBits, ValidBits, IsFloat);
    if (!NT_SUCCESS(status)) { PnMutexRelease(&D->StateMutex); return status; }
    D->AudioRate = Rate;
    D->AudioChannels = Channels;
    D->AudioBits = ContainerBits;
    D->AudioValidBits = ValidBits;
    D->AudioFloat = IsFloat;
    D->AudioStreamOpen = TRUE;
    PnMutexRelease(&D->StateMutex);
    DeviceQueueConnect(D);
    return STATUS_SUCCESS;
}

VOID DeviceAudioStreamClosed(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    KeCancelTimer(&D->SuspendTimer);
    PnMutexAcquire(&D->StateMutex);
    D->AudioRequest = PnAudioStop;
    D->AudioStreamOpen = FALSE;
    PumpStopStream(&D->Pump);
    if (AvdtpGetState(&D->Avdtp) == AvdtpStateStreaming && !D->Removing) AvdtpSuspend(&D->Avdtp);
    PumpReleaseInput(&D->Pump);
    PnMutexRelease(&D->StateMutex);
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
}

VOID DeviceAudioStateChanged(_Inout_ PPNA2DP_DEVICE D, _In_ PN_AUDIO_REQUEST Request)
{
    D->AudioRequest = Request;
    if (D->Removing) return;   /* a stream that outlives a surprise removal: nothing to drive */
    if (Request == PnAudioRun) {
        KeCancelTimer(&D->SuspendTimer);
        QueueAudioWork(D, FALSE);
    } else {
        LARGE_INTEGER due;
        due.QuadPart = -PN_HNS_PER_MS * 3000;      /* SUSPEND after 3 s of silence */
        KeSetTimer(&D->SuspendTimer, due, &D->SuspendDpc);
        QueueAudioWork(D, FALSE);
    }
}

VOID DeviceAudioVolumeChanged(_Inout_ PPNA2DP_DEVICE D, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted)
{
    D->VolumeDb16 = VolumeDb16;
    D->Muted = Muted;
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Config.VolumeDb16 = VolumeDb16;
    KeReleaseSpinLock(&D->StatusLock, irql);
    PumpSetVolume(&D->Pump, D->RemoteVolumeActive ? 0 : VolumeDb16, Muted);
}

/* ------------------------------------------------------------------------- */
/* Control interface -> device                                                  */
/* ------------------------------------------------------------------------- */

NTSTATUS DeviceSetConfig(_Inout_ PPNA2DP_DEVICE D, _In_ const PNA2DP_CONFIG *Cfg)
{
    PAGED_CODE();
    PNA2DP_CONFIG c = *Cfg;
    if (!RegCfgValidate(&c)) return STATUS_INVALID_PARAMETER;
    PnMutexAcquire(&D->StateMutex);
    PNA2DP_CONFIG old = D->Config;
    NTSTATUS saved = RegCfgSaveDevice(D->DriverObject, D->Link.RemoteAddress, &c, D->Link.RemoteName);
    if (!NT_SUCCESS(saved)) { PnMutexRelease(&D->StateMutex); return saved; }
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Config = c;
    KeReleaseSpinLock(&D->StatusLock, irql);
    D->Adapter->SetVolume(ULONG(-1), c.VolumeDb16);

    BOOLEAN codecChanged = RtlCompareMemory(old.CodecPriority, c.CodecPriority, sizeof(c.CodecPriority)) != sizeof(c.CodecPriority) ||
                           old.EnabledCodecMask != c.EnabledCodecMask ||
                           RtlCompareMemory(&old.Sbc, &c.Sbc, sizeof(c.Sbc)) != sizeof(c.Sbc) ||
                           RtlCompareMemory(&old.Aac, &c.Aac, sizeof(c.Aac)) != sizeof(c.Aac) ||
                           RtlCompareMemory(&old.Ldac, &c.Ldac, sizeof(c.Ldac)) != sizeof(c.Ldac) ||
                           RtlCompareMemory(&old.Aptx, &c.Aptx, sizeof(c.Aptx)) != sizeof(c.Aptx) ||
                           RtlCompareMemory(&old.AptxHd, &c.AptxHd, sizeof(c.AptxHd)) != sizeof(c.AptxHd) ||
                           RtlCompareMemory(&old.AptxLl, &c.AptxLl, sizeof(c.AptxLl)) != sizeof(c.AptxLl) ||
                           old.AbrFloorBps != c.AbrFloorBps ||
                           ((old.Flags ^ c.Flags) & (PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_DELAY_REPORTING));
    PumpSetAbr(&D->Pump, (c.Flags & PNA2DP_CFG_ABR_ENABLE) != 0 &&
        (D->Pump.Cfg.Codec != PNA2DP_CODEC_LDAC || c.Ldac.AbrEnable != 0));
    D->Pump.TargetLatencyMs = c.TargetLatencyMs;
    D->Pump.MaxLatencyMs = c.MaxLatencyMs;

    if (codecChanged && D->ActiveValid) {
        /* Live path: if only the SBC bitpool changed, apply in band; otherwise renegotiate. */
        PNA2DP_CONFIG bitpoolOnly = old;
        bitpoolOnly.Sbc.MaxBitpool = c.Sbc.MaxBitpool;
        BOOLEAN onlyBitpool = D->Active.Codec == PNA2DP_CODEC_SBC && old.Sbc.MaxBitpool != c.Sbc.MaxBitpool &&
                              c.Sbc.MaxBitpool >= D->Active.SbcMinBitpool && c.Sbc.MaxBitpool <= D->Active.SbcMaxBitpool &&
                              RtlCompareMemory(&bitpoolOnly, &c, sizeof(c)) == sizeof(c);
        if (onlyBitpool) {
            PumpSetSbcBitpool(&D->Pump, c.Sbc.MaxBitpool);
            D->Active.SbcBitpool = c.Sbc.MaxBitpool;
        } else if (c.Flags & PNA2DP_CFG_APPLY_LIVE) {
            PN_INFO("configuration changed: renegotiating stream");
            BOOLEAN wasRunning = D->AudioRequest == PnAudioRun;
            PumpStopStream(&D->Pump);
            AvdtpClose(&D->Avdtp);
            D->ActiveValid = FALSE;
            if (wasRunning) QueueAudioWork(D, FALSE);
        }
    }
    PnMutexRelease(&D->StateMutex);
    DeviceNotifyEvent(D, PNA2DP_EVENT_CONFIG_CHANGED);
    return STATUS_SUCCESS;
}

NTSTATUS DeviceReconnect(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    PnMutexAcquire(&D->StateMutex);
    PumpStopStream(&D->Pump);
    AvdtpDisconnect(&D->Avdtp);
    D->ActiveValid = FALSE;
    D->ReconnectAttempts = 0;
    PnMutexRelease(&D->StateMutex);
    DeviceQueueConnect(D);
    return STATUS_SUCCESS;
}

VOID DeviceHostChanged(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Removing) return;
    DeviceNotifyEvent(D, PNA2DP_EVENT_HOST_CHANGED);
    /* A better codec may have become available, or the one in use disappeared. */
    QueueAudioWork(D, FALSE);
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                    */
/* ------------------------------------------------------------------------- */

static VOID BuildLocalSeps(_Inout_ PPNA2DP_DEVICE D)
{
    static const ULONG codecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC };
    AVDTP_LOCAL_SEP seps[PN_ARRAYSIZE(codecs)];
    RtlZeroMemory(seps, sizeof(seps));
    for (ULONG i = 0; i < PN_ARRAYSIZE(codecs); i++) {
        seps[i].Seid = (UCHAR)(i + 1);
        seps[i].MediaType = AVDTP_MEDIA_TYPE_AUDIO;
        seps[i].Tsep = AVDTP_TSEP_SOURCE;
        seps[i].Codec = codecs[i];
        seps[i].DelayReporting = TRUE;
        UCHAR mct;
        seps[i].CodecCapsLength = (UCHAR)A2dpBuildLocalCaps(codecs[i], seps[i].CodecCaps, sizeof(seps[i].CodecCaps), &mct);
    }
    AvdtpSetLocalSeps(&D->Avdtp, seps, PN_ARRAYSIZE(codecs));
}

NTSTATUS DeviceCreate(_In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo, _In_ PDRIVER_OBJECT DriverObject, _In_ CPnAdapterCommon *Adapter, _Out_ PPNA2DP_DEVICE *Device)
{
    PAGED_CODE();
    *Device = NULL;
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)PnAllocNp(sizeof(*D), PNA2DP_TAG_DEVICE);
    if (!D) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(D, sizeof(*D));
    D->Signature = PNA2DP_DEVICE_SIGNATURE;
    D->RefCount = 1;
    D->Fdo = Fdo;
    D->Pdo = Pdo;
    D->DriverObject = DriverObject;
    D->Adapter = Adapter;
    ExInitializeFastMutex(&D->StateMutex);
    KeInitializeSpinLock(&D->StatusLock);
    KeInitializeSpinLock(&D->Waiters.Lock);
    InitializeListHead(&D->Waiters.Queue);
    IoCsqInitialize(&D->Waiters.Csq, WCsqInsert, WCsqRemove, WCsqPeek, WCsqAcquire, WCsqRelease, WCsqCancel);
    ExInitializeRundownProtection(&D->WorkRundown);
    ExInitializeRundownProtection(&D->ControlRundown);
    KeInitializeTimer(&D->SuspendTimer);
    KeInitializeDpc(&D->SuspendDpc, SuspendDpc, D);
    KeInitializeTimer(&D->ReconnectTimer);
    KeInitializeDpc(&D->ReconnectDpc, ReconnectDpc, D);
    /* Everything DeviceStop and DeviceTeardown touch is valid from here on, whatever
     * DeviceStart later manages to bring up. */
    CodecHostInitialize(&D->Host, D);
    D->ConnectWorkItem = IoAllocateWorkItem(Fdo);
    D->AudioWorkItem = IoAllocateWorkItem(Fdo);
    D->ScoWorkItem = IoAllocateWorkItem(Fdo);
    RegCfgDefaults(&D->Config);
    if (!D->ConnectWorkItem || !D->AudioWorkItem || !D->ScoWorkItem) {
        DeviceTeardown(D);
        DeviceRelease(D);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    *Device = D;
    return STATUS_SUCCESS;
}

NTSTATUS DeviceStart(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    NTSTATUS status;
    D->WdfDevice = D->Adapter->GetWdfDevice();
    status = PnLinkInitialize(&D->Link, D->WdfDevice, D->Fdo, D->Pdo);
    if (!NT_SUCCESS(status)) return status;
    size_t nameBytes = 0;
    RtlStringCbLengthW(D->Link.RemoteNameW, sizeof(D->Link.RemoteNameW), &nameBytes);
    NTSTATUS nameStatus = IoSetDevicePropertyData(D->Pdo, &DEVPKEY_Device_FriendlyName,
        LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING,
        (ULONG)(nameBytes + sizeof(WCHAR)), D->Link.RemoteNameW);
    if (!NT_SUCCESS(nameStatus)) PN_WARN("device friendly name failed 0x%08x", nameStatus);
    if (!NT_SUCCESS(RegCfgLoadDevice(D->DriverObject, D->Link.RemoteAddress, &D->Config))) {
        RegCfgDefaults(&D->Config);
        RegCfgSaveDevice(D->DriverObject, D->Link.RemoteAddress, &D->Config, D->Link.RemoteName);
    }
    D->VolumeDb16 = D->Config.VolumeDb16;

    status = PumpInitialize(&D->Pump, D);
    if (!NT_SUCCESS(status)) return status;

    AVDTP_CALLBACKS cb;
    RtlZeroMemory(&cb, sizeof(cb));
    cb.Context = D;
    cb.OnRemoteSetConfiguration = OnRemoteSetConfiguration;
    cb.OnRemoteReconfigure = OnRemoteReconfigure;
    cb.OnStreamStateChanged = OnStreamStateChanged;
    cb.OnDelayReport = OnDelayReport;
    cb.OnSignallingDown = OnSignallingDown;
    cb.OnMediaPacket = OnMediaPacket;
    cb.OnMediaDown = OnMediaDown;
    status = AvdtpInitialize(&D->Avdtp, &D->Link, D->Fdo, &cb);
    if (!NT_SUCCESS(status)) return status;
    BuildLocalSeps(D);

    status = PnLinkRegisterServer(&D->Link, OnIncoming, D);
    if (!NT_SUCCESS(status)) PN_WARN("incoming AVDTP connections unavailable (0x%08x)", status);
    PnLinkRegisterRadioEvents(&D->Link, OnSco, OnAcl, D);
    status = ControlRegisterInterface(D);
    if (!NT_SUCCESS(status)) return status;

    KIRQL irql;
    KeAcquireSpinLock(&g_DeviceListLock, &irql);
    InsertTailList(&g_DeviceList, &D->GlobalLink);
    KeReleaseSpinLock(&g_DeviceListLock, irql);

    PumpSetVolume(&D->Pump, D->RemoteVolumeActive ? 0 : D->VolumeDb16, FALSE);
    /* learn the capabilities of the sink early (best effort, in the background) */
    ScheduleReconnect(D, 1500);
    PN_INFO("device started for %012llX (%s)", D->Link.RemoteAddress, D->Link.RemoteName);
    return STATUS_SUCCESS;
}

/* Quiesce. Safe on a device whose DeviceStart failed part way, and safe to call twice.
 * Order matters: first stop every source of new work (IOCTLs, Bluetooth indications,
 * radio events, channels), then wait for the work already queued, and only then cancel
 * the timers, because workers and callbacks are what re-arm them. */
VOID DeviceStop(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    if (D->Stopped) return;
    D->Stopped = TRUE;
    InterlockedExchange(&D->Removing, 1);

    /* no new control handles; wait for IOCTLs in progress */
    if (D->GlobalLink.Flink) {
        KIRQL irql;
        KeAcquireSpinLock(&g_DeviceListLock, &irql);
        RemoveEntryList(&D->GlobalLink);
        D->GlobalLink.Flink = D->GlobalLink.Blink = NULL;
        KeReleaseSpinLock(&g_DeviceListLock, irql);
    }
    ExWaitForRundownProtectionRelease(&D->ControlRundown);
    ControlUnregisterInterface(D);

    /* no new radio events or incoming connections */
    PnLinkUnregisterRadioEvents(&D->Link);
    if (PnLinkUnregisterServer(&D->Link)) {
        /* BthPort may still call the server callback with &D->Link: keep D allocated forever */
        DeviceReference(D);
    }

    /* a worker may be holding StateMutex in an outgoing connect to a speaker that is off */
    if (D->Avdtp.Link) {
        PnChannelCancelConnect(&D->Avdtp.Signalling);
        PnChannelCancelConnect(&D->Avdtp.Media);
    }

    /* stop streaming and close the channels: no more L2CAP callbacks after this */
    PnMutexAcquire(&D->StateMutex);
    PumpStopStream(&D->Pump);
    if (D->Avdtp.Link) {
        if (AvdtpGetState(&D->Avdtp) != AvdtpStateIdle) AvdtpClose(&D->Avdtp);
        AvdtpDisconnect(&D->Avdtp);
    }
    PnMutexRelease(&D->StateMutex);

    /* wait for AVDTP and device work items that were already queued */
    AvdtpStopWork(&D->Avdtp);
    ExWaitForRundownProtectionRelease(&D->WorkRundown);
    KeCancelTimer(&D->SuspendTimer);
    KeCancelTimer(&D->ReconnectTimer);
    KeFlushQueuedDpcs();

    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    for (;;) {
        PIRP irp = IoCsqRemoveNextIrp(&D->Waiters.Csq, NULL);
        if (!irp) break;
        irp->IoStatus.Status = STATUS_DEVICE_REMOVED;
        irp->IoStatus.Information = 0;
        IoCompleteRequest(irp, IO_NO_INCREMENT);
    }
    CodecHostUninitialize(&D->Host);
}

/* Free the subsystems after DeviceStop. The structure itself stays valid until the last
 * reference is released, so late calls from streams or control handles stay harmless. */
VOID DeviceTeardown(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    if (D->TornDown) return;
    DeviceStop(D);
    PnMutexAcquire(&D->StateMutex);
    D->TornDown = TRUE;
    PnMutexRelease(&D->StateMutex);
    if (D->Pump.Device) PumpUninitialize(&D->Pump);
    if (D->Avdtp.Link) AvdtpUninitialize(&D->Avdtp);
    PnLinkUninitialize(&D->Link);
    if (D->ConnectWorkItem) { IoFreeWorkItem(D->ConnectWorkItem); D->ConnectWorkItem = NULL; }
    if (D->AudioWorkItem) { IoFreeWorkItem(D->AudioWorkItem); D->AudioWorkItem = NULL; }
    if (D->ScoWorkItem) { IoFreeWorkItem(D->ScoWorkItem); D->ScoWorkItem = NULL; }
}
