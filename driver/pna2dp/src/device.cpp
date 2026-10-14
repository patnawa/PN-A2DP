/*
 * device.cpp - per-device orchestration: connection state machine, codec selection,
 * audio <-> AVDTP reconciliation, SCO handling, events for user mode.
 *
 * Locking (device.h, avdtp.h): StateMutex -> IntentMutex -> spin locks and StateMutex ->
 * Avdtp.Mutex -> Avdtp.StreamMutex -> spin locks. Only the workers hold StateMutex, and
 * only they wait for Bluetooth I/O; audio-engine and IOCTL threads record intent under
 * the short IntentMutex and queue the worker (T8). AVDTP callbacks run with
 * Avdtp.StreamMutex held and must never take StateMutex or IntentMutex; they record
 * state and queue work items.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"
#include "../../../common/recovery_policy.h"
#include "transport_policy.h"

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

/* Pump start / stop are serialized with the audio endpoint's input changes by IntentMutex
 * (T8). Workers call this with StateMutex held; nothing AVDTP is called inside. */
static VOID StopPump(_Inout_ PPNA2DP_DEVICE D)
{
    PnMutexAcquire(&D->IntentMutex);
    PumpStopStream(&D->Pump);
    PnMutexRelease(&D->IntentMutex);
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

/* PipelineUs / CeilingBps: read from the pump before StatusLock is taken (its own spin
 * locks). C2/C3: the same sources as the presentation position of the wave stream. */
static VOID UpdateStatusLocked(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG PipelineUs, _In_ ULONG CeilingBps)
{
    PNA2DP_STATUS *s = &D->Status;
    s->Size = sizeof(*s);
    s->RemoteAddress = D->Link.RemoteAddress;
    s->State = PnStatusState(AvdtpGetState(&D->Avdtp), D->Avdtp.SignallingUp, PumpIsStreaming(&D->Pump));
    if (D->ActiveValid) {
        s->Codec = D->Active.Codec;
        s->SampleRate = D->Active.SampleRate;
        s->Channels = D->Active.Channels;
        /* a host codec encodes what the pump hands it (LDAC takes S32 on a converted stream) */
        ULONG fmt = PumpIsStreaming(&D->Pump) && D->Pump.Owner == PnEncoderHost ? D->Pump.EncoderPcmFormat : D->Active.PcmFormat;
        s->BitsPerSample = (fmt == PNA2DP_FMT_S16) ? 16 : (fmt == PNA2DP_FMT_S24) ? 24 : 32;
        s->BitrateBps = PumpIsStreaming(&D->Pump) ? PumpCurrentBitrate(&D->Pump) : D->Active.BitrateBps;
        /* while streaming: the pump's effective ceiling (MTU-capped SBC bitpool / AAC fit) */
        s->MaxBitrateBps = PumpIsStreaming(&D->Pump) && CeilingBps ? CeilingBps : D->Active.MaxBitrateBps;
        s->RemoteSeid = D->Avdtp.RemoteSeid;
    } else {
        s->Codec = PNA2DP_CODEC_NONE;
        s->SampleRate = s->Channels = s->BitsPerSample = s->BitrateBps = s->MaxBitrateBps = 0;
    }
    s->MediaMtu = AvdtpMediaMtu(&D->Avdtp);
    /* SinkReportedDelayUs keeps its ABI meaning (the receiver's AVDTP delay report, 0 if none);
     * the end-to-end estimate also includes the per-codec default for non-reporting sinks. */
    ULONG sinkUs = DeviceSinkDelayUs(D);
    s->SinkReportedDelayUs = D->Avdtp.SinkDelayTenthsMs ? sinkUs : 0;
    s->EstimatedLatencyUs = PipelineUs + sinkUs;
    s->AvdtpVersion = D->Avdtp.RemoteVersion;
    s->EncoderOwner = (ULONG)D->Pump.Owner;
    s->Flags = 0;
    if (D->Link.ScoActive) s->Flags |= PNA2DP_STATUS_FLAG_SCO_ACTIVE;
    if (D->Pump.Abr.Enabled && D->Pump.Abr.Level) s->Flags |= PNA2DP_STATUS_FLAG_ABR_ACTIVE;
    if (D->Avdtp.DelayReportingNegotiated) s->Flags |= PNA2DP_STATUS_FLAG_DELAY_REPORTING;
    if (D->Avdtp.RemoteSignallingInitiated) s->Flags |= PNA2DP_STATUS_FLAG_REMOTE_INITIATED;
    if (D->AudioRequest == PnAudioRun) s->Flags |= PNA2DP_STATUS_FLAG_AUDIO_ACTIVE;
    if (D->Link.AclConnected && !D->ReceiverUnreachable) s->Flags |= PNA2DP_STATUS_FLAG_RADIO_IN_RANGE;
    if (D->RemoteVolumeActive) s->Flags |= PNA2DP_STATUS_FLAG_REMOTE_VOLUME;
    if (D->RemoteSuspended) s->Flags |= PNA2DP_STATUS_FLAG_REMOTE_SUSPENDED;
    if (D->ReceiverUnreachable) s->Flags |= PNA2DP_STATUS_FLAG_UNREACHABLE;
    if (PumpIsStreaming(&D->Pump) && D->Pump.Rate.EngineRate != D->Pump.Rate.CodecRate) {
        s->Flags |= PNA2DP_STATUS_FLAG_RATE_CONVERTED;
        if (D->Pump.Rate.Decimate == 2 && !D->Pump.Rate.Rs) s->Flags |= PNA2DP_STATUS_FLAG_HALF_RATE;
    }
    s->EngineSampleRate = D->AudioStreamOpen ? D->AudioRate : 0;
    if (D->Pump.Owner == PnEncoderKernelSbc && D->Config.CodecPriority[0] != PNA2DP_CODEC_SBC && !CodecHostCodecMask(&D->Host)) s->Flags |= PNA2DP_STATUS_FLAG_HOST_MISSING;
}

/* C2: sink delay of the current stream (transport_policy.h): the receiver's DELAYREPORT
 * whenever one was received, even without delay-reporting negotiation (the MR5 sends
 * them unasked), else the documented per-codec default; 0 without a stream.
 * Lock-free, <= DISPATCH. */
ULONG DeviceSinkDelayUs(_In_ PPNA2DP_DEVICE D)
{
    ULONG tenths = D->Avdtp.SinkDelayTenthsMs;
    return PnSinkDelayUs(AvdtpGetState(&D->Avdtp) != AvdtpStateIdle, D->Avdtp.DelayReportingNegotiated, tenths != 0, tenths,
                         D->ActiveValid ? D->Active.Codec : PNA2DP_CODEC_NONE);
}

NTSTATUS DeviceGetStatus(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_STATUS Status)
{
    ULONG pipelineUs = PumpPipelineLatencyUs(&D->Pump);
    ULONG ceilingBps = PumpCeilingBitrate(&D->Pump);
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    UpdateStatusLocked(D, pipelineUs, ceilingBps);
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
    caps.SdpProfileVersion = D->Link.RemoteProfileVersion;
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
    PnAudioFormatsChanged(D);   /* C6: the render pin's formats follow the sink (no-op if unchanged) */
}

/* C6: a configuration with a new codec or rate was negotiated (ours, the receiver's
 * SET_CONFIGURATION, a RECONFIGURE): rebuild the endpoint's format table. Checked at the
 * end of each worker pass (StateMutex), so the AVDTP callbacks stay notification-free. */
static VOID NoteNegotiatedFormat(_Inout_ PPNA2DP_DEVICE D)
{
    ULONG key = D->ActiveValid ? ((D->Active.Codec << 24) ^ D->Active.SampleRate) : 0;
    if (!key || key == D->FormatsKey) return;
    D->FormatsKey = key;
    PnAudioFormatsChanged(D);
}

/* C6: the endpoint's jack follows the receiver's presence (PnJackAction). <= DISPATCH. */
static VOID JackEvent(_Inout_ PPNA2DP_DEVICE D, _In_ PN_JACK_EVENT Event)
{
    ULONG action = PnJackAction(Event, D->UserDisconnected != 0);
    if (action != PN_JACK_KEEP) PnAudioSetJackConnected(D, action == PN_JACK_PLUG);
}

/* Codecs the codec host can encode right now (PNA2DP_CODEC_BIT mask). */
static ULONG HostEncoderMask(_In_ PPNA2DP_DEVICE D)
{
    ULONG mask = 0;
    for (ULONG c = PNA2DP_CODEC_SBC; c < PNA2DP_CODEC_MAX; c++) if (CodecHostAvailable(&D->Host, c)) mask |= PNA2DP_CODEC_BIT(c);
    return mask;
}

static BOOLEAN EncoderAvailable(_In_ PPNA2DP_DEVICE D, _In_ ULONG Codec, _Out_ PN_ENCODER_OWNER *Owner)
{
    UCHAR owner;
    BOOLEAN ok = PnEncoderFor(Codec, D->Config.Flags, HostEncoderMask(D), &owner);
    *Owner = (PN_ENCODER_OWNER)owner;
    return ok;
}

static BOOLEAN ConservativeRadio(_In_ PPNA2DP_DEVICE D)
{
    return (D->Config.Flags & PNA2DP_CFG_AUTO_QUALITY) &&
        PnAdapterNeedsConservativeProfile(D->Link.RadioInfoValid, D->Link.RadioInfo.radioInfo.lmpSupportedFeatures);
}

/* C8 / #1: the engine rates the pump bridges to another stream rate (PN_RATECONV_*): with the
 * sample-rate converter any of 44.1 / 48 / 88.2 / 96 kHz feeds a 44.1 or 48 kHz stream, so the
 * codec choice follows the priority list rather than the Windows format. */
static ULONG DeviceRateConvert(_In_ PPNA2DP_DEVICE D)
{
    UNREFERENCED_PARAMETER(D);
    return PN_PUMP_RATE_CONVERT;
}

/* The refused (codec, SEID) pairs and the codec upgrade guard belong to one signalling
 * session (StateMutex held). */
static VOID SessionCheck(_Inout_ PPNA2DP_DEVICE D)
{
    ULONG gen = D->Avdtp.Signalling.Generation;
    if (gen == D->SessionGen) return;
    D->SessionGen = gen;
    RtlZeroMemory(&D->Rejected, sizeof(D->Rejected));
    D->CodecUpgradeTried = FALSE;
}

/* T10: DISCOVER reports the local SEP of a codec that is off or has no encoder in use, so
 * a receiver does not configure it only to be refused. SBC always has the kernel encoder.
 * <= DISPATCH (codec host notifications). */
static VOID RefreshLocalSepAvailability(_Inout_ PPNA2DP_DEVICE D)
{
    if (!D->Avdtp.Link) return;
    ULONG host = CodecHostCodecMask(&D->Host);
    ULONG enabled = D->Config.EnabledCodecMask;
    for (ULONG c = PNA2DP_CODEC_SBC; c < PNA2DP_CODEC_MAX && c < 32; c++) {
        BOOLEAN available = c == PNA2DP_CODEC_SBC || ((enabled & PNA2DP_CODEC_BIT(c)) && (host & PNA2DP_CODEC_BIT(c)));
        AvdtpSetLocalSepAvailable(&D->Avdtp, c, available);
    }
}

/* Picks codec/config according to the priority list (pure: transport_policy.h). */
static BOOLEAN ChooseConfig(_Inout_ PPNA2DP_DEVICE D, _Out_ PN_CODEC_CHOICE *Choice)
{
    PN_CODEC_CHOICE_INPUT in;
    RtlZeroMemory(&in, sizeof(in));
    SessionCheck(D);
    in.RemoteSeps = D->Avdtp.RemoteSeps;
    in.RemoteSepCount = D->Avdtp.RemoteSepCount;
    in.LocalSeps = D->Avdtp.LocalSeps;
    in.LocalSepCount = D->Avdtp.LocalSepCount;
    in.Config = &D->WorkConfig;
    in.HostCodecMask = HostEncoderMask(D);
    in.RestrictRate = D->AudioStreamOpen;
    in.PreferredRate = D->AudioStreamOpen ? D->AudioRate : 48000;
    in.PreferredBits = D->AudioStreamOpen ? D->AudioValidBits : 16;
    in.Conservative = ConservativeRadio(D);
    in.RateConvert = DeviceRateConvert(D);
    in.OwnRemoteSeid = D->Avdtp.RemoteSeid;   /* T1: our own (last) stream's SEP is not "in use" for us */
    in.Rejected = &D->Rejected;
    return PnChooseCodecConfig(&in, Choice);
}

static ULONG AgeMs(_In_ LONGLONG SinceHns)
{
    LONGLONG d = (PnNowHns() - SinceHns) / PN_HNS_PER_MS;
    return d < 0 ? 0 : d > 0x7FFFFFFF ? 0x7FFFFFFF : (ULONG)d;
}

static ULONGLONG NowMs(void) { return (ULONGLONG)PnNowHns() / PN_HNS_PER_MS; }

/* ------------------------------------------------------------------------- */
/* Reconcile: make reality match AudioRequest (PASSIVE, StateMutex held)        */
/* ------------------------------------------------------------------------- */

static VOID ScheduleReconnect(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG DelayMs)
{
    if (D->Removing || D->PowerSuspended) return;
    LARGE_INTEGER due;
    due.QuadPart = -PN_HNS_PER_MS * (LONGLONG)DelayMs;
    KeSetTimer(&D->ReconnectTimer, due, &D->ReconnectDpc);
}

/* T5: the AVDTP version from SDP, whoever opened signalling, until one query succeeds; a
 * failed one is tried again in a later signalling session, a few times per device
 * (PnShouldQuerySdp, review #5). Best effort; called without StateMutex by the workers
 * and, for a session we connected ourselves, from EnsureSignalling before discovery (as
 * before). Needs a link to talk over, so a speaker that is off is not paged for it.
 * PASSIVE. */
static VOID QuerySdpOnce(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Removing || D->PowerSuspended) return;
    ULONG session = D->Avdtp.Signalling.Generation;
    if (!PnShouldQuerySdp(D->SdpSucceeded, D->SdpFailures, D->SdpFailures && D->SdpFailedSession == session)) return;
    if (!D->Avdtp.SignallingUp && !D->Link.AclConnected) return;
    if (InterlockedCompareExchange(&D->SdpBusy, 1, 0) != 0) return;
    /* decided again by the owner: another worker may have finished a query meanwhile */
    if (PnShouldQuerySdp(D->SdpSucceeded, D->SdpFailures, D->SdpFailures && D->SdpFailedSession == session)) {
        NTSTATUS status = PnLinkQueryRemoteSdp(&D->Link, A2DP_UUID_AUDIO_SINK);
        if (NT_SUCCESS(status)) {
            D->SdpSucceeded = TRUE;
        } else {
            D->SdpFailures++;
            D->SdpFailedSession = session;
            PN_WARN("SDP query failed 0x%08x (attempt %u); AVDTP version stays %04X", status, D->SdpFailures, D->Avdtp.RemoteVersion);
        }
    }
    InterlockedExchange(&D->SdpBusy, 0);
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
    QuerySdpOnce(D);
    D->ReconnectAttempts = 0;
    D->ReceiverUnreachable = FALSE;
    JackEvent(D, PnJackSignallingUp);
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    return STATUS_SUCCESS;
}

static NTSTATUS EnsureDiscovered(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Avdtp.RemoteSepsValid) return STATUS_SUCCESS;
    /* the SDP answer may have arrived after an earlier discovery: use it from now on */
    if (!D->SdpVersionApplied && D->Link.RemoteAvdtpVersion) {
        D->Avdtp.RemoteVersion = D->Link.RemoteAvdtpVersion;
        D->SdpVersionApplied = TRUE;
    }
    NTSTATUS status = AvdtpDiscover(&D->Avdtp);
    if (NT_SUCCESS(status)) RefreshCapabilities(D);
    else D->Status.LastError = (ULONG)status;
    return status;
}

/* The encoder gain: unity while Windows drives the receiver's absolute volume; after that
 * hand-off ends, the user's volume less what the receiver still applies (#2/#27). */
static LONG SoftwareVolumeDb16(_In_ PPNA2DP_DEVICE D, _In_ LONG UserDb16)
{
    return PnSoftwareVolumeDb16(D->RemoteVolumeActive, D->ReceiverHoldsVolume, D->ReceiverHeldDb16, UserDb16);
}

static VOID SetActiveConfig(_Inout_ PPNA2DP_DEVICE D, _In_opt_ const A2DP_CODEC_CONFIG *Cfg)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    if (Cfg) D->Active = *Cfg;
    D->ActiveValid = Cfg != NULL;
    D->ActiveStreamed = FALSE;   /* a new configuration has carried no audio yet */
    KeReleaseSpinLock(&D->StatusLock, irql);
}

/* SET_CONFIGURATION down the codec priority list (T1, T4): a refused (codec, SEID) pair is
 * remembered for the session and the next choice is tried, SEP_IN_USE first refreshes the
 * in-use bits with one DISCOVER, and a refusal of delay reporting is retried without it. */
#define PN_SETCONFIG_MAX_ATTEMPTS 8
static NTSTATUS ConfigureStream(_Inout_ PPNA2DP_DEVICE D)
{
    BOOLEAN rediscovered = FALSE;
    NTSTATUS status = STATUS_NOT_SUPPORTED;
    for (ULONG attempt = 0; attempt < PN_SETCONFIG_MAX_ATTEMPTS; attempt++) {
        PN_CODEC_CHOICE choice;
        if (!ChooseConfig(D, &choice)) {
            PN_ERR("no usable codec/SEP on the sink");
            /* the next attempt starts down the list again */
            RtlZeroMemory(&D->Rejected, sizeof(D->Rejected));
            D->Status.LastError = (ULONG)STATUS_NOT_SUPPORTED;
            return STATUS_NOT_SUPPORTED;
        }
        A2DP_CODEC_CONFIG cfg = choice.Cfg;
        BOOLEAN delay = PnRequestDelayReporting(D->Config.Flags, choice.ServiceCaps, D->Avdtp.RemoteVersion);
        PN_INFO("configuring %s @ %u Hz, chmode %u, local seid %u -> remote seid %u (owner %u)", A2dpCodecName(cfg.Codec), cfg.SampleRate, cfg.ChannelMode, choice.LocalSeid, choice.RemoteSeid, choice.Owner);
        UCHAR ec = 0;
        status = AvdtpSetConfiguration(&D->Avdtp, choice.LocalSeid, choice.RemoteSeid, cfg.Ies, cfg.IesLength, delay, FALSE, &ec);
        PN_SETCONFIG_ACTION action = NT_SUCCESS(status) ? PnSetConfigFail : PnSetConfigFailureAction(status, ec, delay, rediscovered);
        if (action == PnSetConfigRetryWithoutDelay) {
            PN_WARN("retrying SET_CONFIGURATION without delay reporting");
            status = AvdtpSetConfiguration(&D->Avdtp, choice.LocalSeid, choice.RemoteSeid, cfg.Ies, cfg.IesLength, FALSE, FALSE, &ec);
            action = NT_SUCCESS(status) ? PnSetConfigFail : PnSetConfigFailureAction(status, ec, FALSE, rediscovered);
        }
        if (NT_SUCCESS(status)) {
            SetActiveConfig(D, &cfg);   /* StartPumpIfNeeded picks the encoder owner again */
            D->RemoteConfigured = FALSE;
            return STATUS_SUCCESS;
        }
        if (action == PnSetConfigRediscover) {
            PN_WARN("remote seid %u in use: discovering again", choice.RemoteSeid);
            rediscovered = TRUE;
            NTSTATUS ds = AvdtpDiscover(&D->Avdtp);
            if (!NT_SUCCESS(ds)) { status = ds; break; }
            RefreshCapabilities(D);
            continue;
        }
        if (action == PnSetConfigTryNext) {
            PN_WARN("%s on remote seid %u refused (0x%02X): trying the next codec", A2dpCodecName(cfg.Codec), choice.RemoteSeid, ec);
            PnRejectedAdd(&D->Rejected, cfg.Codec, choice.RemoteSeid);
            continue;
        }
        break;
    }
    D->Status.LastError = (ULONG)status;
    return status;
}

/* LDAC needs a 679-byte media MTU (libldac): below it every encode would fail and the codec
 * host be quarantined. An open LDAC stream on a smaller MTU is refused for this session and
 * closed (finding #18); TRUE when it was. */
static BOOLEAN RefuseLdacForMtu(_Inout_ PPNA2DP_DEVICE D)
{
    if (!D->ActiveValid || D->Active.Codec != PNA2DP_CODEC_LDAC || PnLdacMediaMtuUsable(AvdtpMediaMtu(&D->Avdtp))) return FALSE;
    PN_WARN("media MTU %u is too small for LDAC; selecting another codec", AvdtpMediaMtu(&D->Avdtp));
    PnRejectedAdd(&D->Rejected, PNA2DP_CODEC_LDAC, D->Avdtp.RemoteSeid);
    AvdtpClose(&D->Avdtp);
    D->ActiveValid = FALSE;
    return TRUE;
}

/* STATUS_PENDING: the receiver completes the open, or (WaitMs set) we give its OPEN time
 * after its SET_CONFIGURATION first. */
static NTSTATUS OpenStream(_Inout_ PPNA2DP_DEVICE D, _Out_opt_ PULONG WaitMs)
{
    if (WaitMs) *WaitMs = 0;
    AVDTP_STREAM_STATE st = AvdtpGetState(&D->Avdtp);
    /* T3/T9 at the OPEN itself: the receiver may have configured the stream during our
     * discovery, after this pass planned (review round 5 #3) */
    ULONG wait = PnRemoteOpenWaitMs(D->RemoteConfigured, st, AgeMs(D->RemoteConfiguredHns));
    if (wait) {
        if (WaitMs) *WaitMs = wait;
        return STATUS_PENDING;
    }
    if (st == AvdtpStateIdle) {
        NTSTATUS status = ConfigureStream(D);
        if (!NT_SUCCESS(status)) return status;
        st = AvdtpStateConfigured;
    }
    if (st == AvdtpStateConfigured) {
        UCHAR ec = 0;
        BOOLEAN collided = FALSE;
        NTSTATUS status = AvdtpOpen(&D->Avdtp, &ec, &collided);
        if (NT_SUCCESS(status)) D->OpenCollisions = 0;
        if (status == STATUS_PENDING) return STATUS_PENDING;   /* the receiver's transport completes the open */
        if (!NT_SUCCESS(status)) {
            D->Status.LastError = (ULONG)status;
            PN_OPEN_ACTION action = PnOpenFailureAction(status, ec, collided, D->OpenCollisions);
            D->OpenCollisions = action == PnOpenFailCollision ? D->OpenCollisions + 1 : 0;
            switch (action) {
            case PnOpenFailCollision:
                /* T9: both sides sent OPEN at once and each refused the other's (our
                 * acceptor refused the receiver's). Ask again after the setup back-off, or
                 * the receiver's own retry opens it; a second collision in a row aborts
                 * (review #3) */
                PN_INFO("OPEN collided with the receiver's (state %u)", AvdtpGetState(&D->Avdtp));
                PnSetupRetryFailed(&D->SetupRetry);   /* a receiver that never opens is not asked every 1.5 s */
                return STATUS_PENDING;
            case PnOpenFailAbort:
                /* the sink no longer has this stream configured: abort to Idle so the next
                 * attempt sends SET_CONFIGURATION again instead of repeating OPEN */
                AvdtpAbort(&D->Avdtp);
                SetActiveConfig(D, NULL);
                break;
            default:
                break;
            }
            return status;
        }
        st = AvdtpStateOpen;
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    }
    if (st == AvdtpStateOpening) return STATUS_PENDING;   /* remote is opening the transport */
    return STATUS_SUCCESS;
}

/* OpenStream; an LDAC stream whose media MTU turns out too small is refused as soon as it is
 * open, before START, and the next codec is configured and opened in the same pass instead
 * of after a START, a CLOSE and the setup back-off (finding #11). */
static NTSTATUS EnsureConfiguredAndOpen(_Inout_ PPNA2DP_DEVICE D, _Out_opt_ PULONG WaitMs)
{
    NTSTATUS status = OpenStream(D, WaitMs);
    if (status != STATUS_SUCCESS || !RefuseLdacForMtu(D)) return status;
    if (AvdtpGetState(&D->Avdtp) != AvdtpStateIdle) return STATUS_DEVICE_NOT_READY;   /* the close did not finish: as before */
    status = OpenStream(D, WaitMs);   /* LDAC is refused for the session now: the next choice */
    if (status == STATUS_SUCCESS && RefuseLdacForMtu(D)) return STATUS_DEVICE_NOT_READY;
    return status;
}

/* IntentGen: the audio intent this pass planned for. STATUS_CANCELLED: the endpoint closed
 * or reopened, or a renegotiation / reset was requested since; the pass that intent queued
 * decides (T8). */
static NTSTATUS StartPumpIfNeeded(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG IntentGen)
{
    if (PumpIsStreaming(&D->Pump)) return STATUS_SUCCESS;
    if (D->IntentGen != IntentGen) return STATUS_CANCELLED;
    if (!D->ActiveValid || !D->AudioStreamOpen) return STATUS_INVALID_DEVICE_STATE;
    PN_ENCODER_OWNER owner;
    if (!EncoderAvailable(D, D->Active.Codec, &owner)) {
        PN_WARN("encoder for %s vanished; closing stream to renegotiate", A2dpCodecName(D->Active.Codec));
        PnLogDeviceEvent(D->Fdo, PN_EVENT_CODEC_HOST, PN_EVENT_CODEC_HOST_FALLBACK | (D->Active.Codec & 0xFFFF));
        AvdtpClose(&D->Avdtp);
        D->ActiveValid = FALSE;
        return STATUS_DEVICE_NOT_READY;
    }
    /* LDAC on a too-small media MTU (finding #18): normally refused at OPEN (#11); a stream the
     * receiver opened and started itself is refused here and renegotiated */
    if (RefuseLdacForMtu(D)) return STATUS_DEVICE_NOT_READY;
    /* SBC XQ packs to the EDR packets the receiver takes too (review round 4 #5);
     * queried outside IntentMutex, cached for the link */
    if (D->Active.Codec == PNA2DP_CODEC_SBC && (D->WorkConfig.Flags & PNA2DP_CFG_SBC_XQ)) PnLinkQueryRemoteFeatures(&D->Link);
    PnMutexAcquire(&D->IntentMutex);
    if (D->IntentGen != IntentGen || !D->AudioStreamOpen) { PnMutexRelease(&D->IntentMutex); return STATUS_CANCELLED; }
    NTSTATUS status = PumpStartStream(&D->Pump, &D->Active, AvdtpMediaChannel(&D->Avdtp), AvdtpMediaMtu(&D->Avdtp), owner, &D->WorkConfig);
    PumpSetVolume(&D->Pump, SoftwareVolumeDb16(D, D->VolumeDb16), D->Muted);
    if (NT_SUCCESS(status)) {
        InterlockedExchange64(&D->RenderHoldUntilHns, 0);   /* the clock runs: the pump takes the audio */
        D->ActiveStreamed = TRUE;   /* from now on a Windows rate change is bridged (PnStreamBridged) */
    }
    PnMutexRelease(&D->IntentMutex);
    if (!NT_SUCCESS(status)) D->Status.LastError = (ULONG)status;
    return status;
}

/* A configured stream whose format no longer matches the audio engine (pure decision:
 * PnPlanStreamFormat). Returns the decision; WaitOpenReconfigure leaves the stream for
 * the receiver's (or our) OPEN and is checked again before START. */
static PN_FORMAT_ACTION CheckStreamFormat(_Inout_ PPNA2DP_DEVICE D)
{
    PN_FORMAT_INPUT in;
    A2DP_CODEC_CONFIG cfg;
    AVDTP_STREAM_STATE st = AvdtpGetState(&D->Avdtp);
    const AVDTP_REMOTE_SEP *r = NULL;
    for (ULONG i = 0; i < D->Avdtp.RemoteSepCount; i++) if (D->Avdtp.RemoteSeps[i].Seid == D->Avdtp.RemoteSeid) r = &D->Avdtp.RemoteSeps[i];
    in.ActiveValid = D->ActiveValid;
    in.Active = &D->Active;
    in.Remote = r;
    in.Config = &D->WorkConfig;
    in.AudioStreamOpen = D->AudioStreamOpen;
    in.AudioRate = D->AudioRate;
    in.AudioValidBits = D->AudioValidBits;
    in.Conservative = ConservativeRadio(D);
    in.StreamState = st;
    in.RemoteConfigured = D->RemoteConfigured;
    in.RateConvert = DeviceRateConvert(D);
    in.Streamed = D->ActiveStreamed;
    PN_FORMAT_ACTION action = PnPlanStreamFormat(&in, &cfg);
    switch (action) {
    case PnFormatReconfigure: {
        PN_INFO("reconfiguring stream to %u Hz, channel mode %u for saved preferences", cfg.SampleRate, cfg.ChannelMode);
        if (PumpIsStreaming(&D->Pump)) StopPump(D);
        if (st == AvdtpStateStreaming) AvdtpSuspend(&D->Avdtp);
        NTSTATUS rs = (AvdtpGetState(&D->Avdtp) == AvdtpStateOpen) ? AvdtpReconfigure(&D->Avdtp, cfg.Ies, cfg.IesLength) : STATUS_INVALID_DEVICE_STATE;
        if (!NT_SUCCESS(rs)) {
            AvdtpClose(&D->Avdtp);   /* CLOSE, or ABORT for a stream that is only configured (T3) */
            D->ActiveValid = FALSE;
        } else {
            cfg.BitrateBps = cfg.MaxBitrateBps = A2dpNominalBitrate(&cfg);
            D->Active = cfg;
            D->ActiveStreamed = FALSE;
        }
        break;
    }
    case PnFormatReselect:
        PN_WARN("sink does not support %u Hz for %s; selecting another codec", D->AudioRate, A2dpCodecName(D->Active.Codec));
        StopPump(D);
        AvdtpClose(&D->Avdtp);
        D->ActiveValid = FALSE;
        break;
    case PnFormatWaitOpenReconfigure:
        PN_INFO("receiver configured %u Hz; reconfiguring to %u Hz once the stream is open", D->Active.SampleRate, cfg.SampleRate);
        break;
    default:
        /* #1: the pump converts; logged when the stream (re)starts, not on every pass */
        if (D->ActiveValid && D->AudioStreamOpen && D->AudioRate != D->Active.SampleRate && !PumpIsStreaming(&D->Pump))
            PN_INFO("engine %u Hz bridged to the %u Hz stream", D->AudioRate, D->Active.SampleRate);
        break;
    }
    return action;
}

/* Snapshot for PnPlanReconcile. The one-shot requests are consumed only by a pass that
 * can act on them. */
static VOID ReconcileSnapshot(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN SuspendDue, _Out_ PN_RECONCILE_INPUT *In)
{
    RtlZeroMemory(In, sizeof(*In));
    SessionCheck(D);
    In->Removing = D->Removing != 0;
    In->PowerSuspended = D->PowerSuspended != 0;
    In->PumpStreaming = PumpIsStreaming(&D->Pump);
    if (!In->Removing && !In->PowerSuspended) {
        In->ResetBudget = InterlockedExchange(&D->ResetReconnectBudget, 0) != 0;
        In->PowerResumePending = PnPowerResetOnResume(InterlockedExchange(&D->PowerResumePending, 0) != 0,
                                                      D->Avdtp.SignallingUp && PnChannelIsConnected(&D->Avdtp.Signalling),
                                                      D->Link.AclConnected);
        In->RemoteStarted = InterlockedExchange(&D->RemoteStartedPending, 0) != 0;
        /* a stall matters only for a running pump; a link loss only while the ACL is still down */
        In->MediaStalled = InterlockedExchange(&D->MediaStallPending, 0) != 0 && In->PumpStreaming;
        In->LinkLost = InterlockedExchange(&D->LinkLostPending, 0) != 0 && !D->Link.AclConnected;
    }
    In->AudioRun = D->AudioRequest == PnAudioRun;
    In->AudioStreamOpen = D->AudioStreamOpen;
    In->ScoActive = D->Link.ScoActive;
    In->SuspendDue = SuspendDue;
    if (D->PowerIdle) {   /* T11: Dx in S0: SUSPEND, keep the session */
        In->AudioRun = FALSE;
        In->SuspendDue = TRUE;
    }
    In->ConfigFlags = D->Config.Flags;
    In->StreamState = AvdtpGetState(&D->Avdtp);
    In->PumpOwnerHost = D->Pump.Owner == PnEncoderHost;
    In->HostPresent = CodecHostCodecMask(&D->Host) != 0;
    In->SignallingUp = D->Avdtp.SignallingUp;
    In->RemoteSuspended = D->RemoteSuspended != 0;
    In->LocalStartRequested = D->LocalStartRequested != 0;
    In->RemoteSuspendAgeMs = AgeMs(D->RemoteSuspendHns);
    In->RemoteSuspendRetried = D->RemoteSuspendRetried != 0;
    In->RemoteConfigured = D->RemoteConfigured;
    In->RemoteConfiguredAgeMs = AgeMs(D->RemoteConfiguredHns);
    In->CodecUpgradeTried = D->CodecUpgradeTried;
    In->RemoteSepsUnknown = !D->Avdtp.RemoteSepsValid;
    In->UserDisconnected = D->UserDisconnected != 0;
    /* T10: before discovery the answer is unknown; the step decides after discovery.
     * G9: a stream of ours on a lower-ranked codec (the SBC fallback while the codec host
     * was quarantined or away) switches back once the chooser picks a better one; the
     * planner does it at the next START or pause, once per session. */
    if (!D->CodecUpgradeTried && D->ActiveValid && (D->RemoteConfigured || D->Avdtp.RemoteSepsValid)) {
        PN_CODEC_CHOICE c;
        In->BetterCodecAvailable = (D->RemoteConfigured && !D->Avdtp.RemoteSepsValid) ||
            (ChooseConfig(D, &c) && PnCodecRank(&D->WorkConfig, c.Cfg.Codec) < PnCodecRank(&D->WorkConfig, D->Active.Codec));
    }
    In->SetupFailures = D->SetupRetry.failures;
}

typedef struct _PN_RECONCILE_PASS {
    BOOLEAN RetryArmed;       /* this pass scheduled the next one */
    BOOLEAN FormatPending;    /* a receiver-configured format waits for the stream to open */
    ULONG   IntentGen;        /* D->IntentGen this pass planned for (T8) */
} PN_RECONCILE_PASS;

/* T8: carry out what audio-engine and IOCTL threads recorded without waiting for
 * Bluetooth I/O themselves, and take this pass's copy of the configuration. StateMutex
 * held. A request waits while the device is powered down or going away (DeviceStop
 * closes everything itself). */
static VOID ApplyIntents(_Inout_ PPNA2DP_DEVICE D, _Inout_ PN_RECONCILE_PASS *Pass)
{
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->WorkConfig = D->Config;
    KeReleaseSpinLock(&D->StatusLock, irql);
    Pass->IntentGen = D->IntentGen;
    if (D->Removing || D->PowerSuspended) return;
    if (InterlockedExchange(&D->UserResetPending, 0)) {
        PN_INFO("reconnect requested");
        InterlockedExchange(&D->RenegotiatePending, 0);   /* the reset covers it */
        StopPump(D);
        AvdtpDisconnect(&D->Avdtp);
        D->ActiveValid = FALSE;
        D->ReconnectAttempts = 0;
        D->SetupRetry.failures = 0;   /* a user request starts the back-off over */
        D->ConsecutiveTimeouts = 0;
    }
    if (InterlockedExchange(&D->UserConnectPending, 0)) {
        /* ONESHOT_RECONNECT: a user request starts the back-off over; a working session
         * is kept (review #12) */
        D->ReconnectAttempts = 0;
        D->SetupRetry.failures = 0;
    }
    if (InterlockedExchange(&D->UserDisconnectPending, 0) && D->UserDisconnected) {
        /* C7: Windows disconnected the receiver: drop the session and stay disconnected
         * until ONESHOT_RECONNECT, a new local playback start or the receiver connects */
        PN_INFO("disconnect requested");
        InterlockedExchange(&D->RenegotiatePending, 0);   /* the disconnect covers it */
        KeCancelTimer(&D->ReconnectTimer);
        StopPump(D);
        AvdtpDisconnect(&D->Avdtp);
        SetActiveConfig(D, NULL);
        D->ReconnectAttempts = 0;
        JackEvent(D, PnJackUserDisconnect);
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
    }
    if (InterlockedExchange(&D->RenegotiatePending, 0)) {
        PN_INFO("configuration changed: renegotiating stream");
        StopPump(D);
        AvdtpClose(&D->Avdtp);   /* CLOSE, or ABORT for a stream that is only configured */
        D->ActiveValid = FALSE;
    }
}

static VOID ArmRetry(_Inout_ PPNA2DP_DEVICE D, _Inout_ PN_RECONCILE_PASS *Pass, _In_ ULONG DelayMs)
{
    ScheduleReconnect(D, DelayMs);
    Pass->RetryArmed = TRUE;
}

/* A step failed: notify, suspend, abort, drop and retry as PnReconcileOnFailure decides
 * (T4). The setup-failure counter backs the retries off until streaming holds. */
static VOID ReconcileFailed(_Inout_ PPNA2DP_DEVICE D, _Inout_ PN_RECONCILE_PASS *Pass, _In_ PN_RECONCILE_STEP Step, _In_ NTSTATUS Status)
{
    PN_FAILURE_INPUT f;
    PN_SETUP_FAILURE a;
    RtlZeroMemory(&f, sizeof(f));
    PnSetupRetryObserve(&D->SetupRetry, NowMs(), 0);   /* a long stream that just ended resets the count */
    f.Step = Step;
    f.Status = Status;
    f.ReconnectAttempts = D->ReconnectAttempts;
    f.ReconnectDelayMs = D->Config.ReconnectDelayMs;
    f.SetupFailures = D->SetupRetry.failures;
    f.ConsecutiveTimeouts = D->ConsecutiveTimeouts;
    PnReconcileOnFailure(&f, &a);
    D->ConsecutiveTimeouts = Status == STATUS_IO_TIMEOUT ? D->ConsecutiveTimeouts + 1 : 0;
    if (a.SuspendIfStreaming && AvdtpGetState(&D->Avdtp) == AvdtpStateStreaming) AvdtpSuspend(&D->Avdtp);
    if (a.NotifyError) DeviceNotifyEvent(D, PNA2DP_EVENT_ERROR);
    if (a.AbortStream) {
        StopPump(D);
        AvdtpAbort(&D->Avdtp);
        SetActiveConfig(D, NULL);
    }
    if (a.Disconnect) {
        StopPump(D);
        AvdtpDisconnect(&D->Avdtp);
        SetActiveConfig(D, NULL);
        D->ConsecutiveTimeouts = 0;
    }
    if (a.RetryDelayMs) {
        if (a.BumpReconnectAttempts && D->ReconnectAttempts < 10) D->ReconnectAttempts++;
        PnSetupRetryFailed(&D->SetupRetry);
        PN_INFO("stream setup failed at step %u (0x%08x); retry in %u ms", (ULONG)Step, Status, a.RetryDelayMs);
        ArmRetry(D, Pass, a.RetryDelayMs);
    }
}

/* Executes one planned step. FALSE ends the pass. */
static BOOLEAN ReconcileStep(_Inout_ PPNA2DP_DEVICE D, _Inout_ PN_RECONCILE_PASS *Pass, _In_ PN_RECONCILE_STEP Step)
{
    NTSTATUS status = STATUS_SUCCESS;
    switch (Step) {
    case PnStepStopPump:
        StopPump(D);
        return TRUE;
    case PnStepResetAttempts:
        D->ReconnectAttempts = 0;
        return TRUE;
    case PnStepResetSession:
        StopPump(D);
        AvdtpDisconnect(&D->Avdtp);
        D->ActiveValid = FALSE;
        D->ReconnectAttempts = 0;
        return TRUE;
    case PnStepCloseForHostLoss:
        PN_WARN("codec host lost while streaming %s; renegotiating", A2dpCodecName(D->Active.Codec));
        AvdtpClose(&D->Avdtp);
        D->ActiveValid = FALSE;
        return TRUE;
    case PnStepEnsureSignalling:
        status = EnsureSignalling(D);
        break;
    case PnStepEnsureDiscovered:
        status = EnsureDiscovered(D);
        break;
    case PnStepCheckFormat:
        if (CheckStreamFormat(D) == PnFormatWaitOpenReconfigure) Pass->FormatPending = TRUE;
        return TRUE;
    case PnStepUpgradeCodec: {
        /* T10: once per session, switch a receiver-chosen codec to a better one that fits */
        PN_CODEC_CHOICE c;
        if (!D->Avdtp.RemoteSepsValid) return TRUE;   /* nothing to choose from yet: keep the try */
        D->CodecUpgradeTried = TRUE;
        if (!D->ActiveValid || !ChooseConfig(D, &c) || PnCodecRank(&D->WorkConfig, c.Cfg.Codec) >= PnCodecRank(&D->WorkConfig, D->Active.Codec))
            return TRUE;
        PN_INFO("receiver chose %s; switching to %s", A2dpCodecName(D->Active.Codec), A2dpCodecName(c.Cfg.Codec));
        StopPump(D);
        AvdtpClose(&D->Avdtp);
        SetActiveConfig(D, NULL);
        D->RemoteConfigured = FALSE;
        Pass->FormatPending = FALSE;
        return TRUE;
    }
    case PnStepEnsureOpen: {
        ULONG wait = 0;
        status = EnsureConfiguredAndOpen(D, &wait);
        if (status == STATUS_PENDING) {
            /* the remote is opening the transport; or its OPEN after its SET_CONFIGURATION
             * gets the rest of its time, as PnStepWaitRemoteOpen gives it */
            if (wait) ArmRetry(D, Pass, wait);
            return FALSE;
        }
        break;
    }
    case PnStepStartIfOpen:
        if (Pass->FormatPending && AvdtpGetState(&D->Avdtp) == AvdtpStateOpen) {
            Pass->FormatPending = FALSE;
            CheckStreamFormat(D);   /* now RECONFIGURE is possible */
        }
        if (AvdtpGetState(&D->Avdtp) != AvdtpStateOpen) return TRUE;
        /* A media channel may stay open while its ACL goes into sniff mode.
         * Wake the link before the latency-sensitive START transaction. */
        PnLinkEnsureActive(&D->Link);
        if (D->RemoteSuspended && !D->LocalStartRequested) {
            /* T2: the back-off's one START of this suspend episode; refused or suspended
             * again, the next is up to playback here or the receiver (review round 3 #3) */
            D->RemoteSuspendRetryHns = PnNowHns();
            InterlockedExchange(&D->RemoteSuspendRetried, 1);
        }
        status = AvdtpStart(&D->Avdtp);
        if (NT_SUCCESS(status)) {
            InterlockedExchange(&D->RemoteSuspended, 0);
            InterlockedExchange(&D->LocalStartRequested, 0);
        }
        break;
    case PnStepStartPump:
        if (!PnPumpMayRun(AvdtpGetState(&D->Avdtp))) return FALSE;
        status = StartPumpIfNeeded(D, Pass->IntentGen);
        if (status == STATUS_CANCELLED) return FALSE;   /* T8: a newer intent; its queued pass decides */
        if (NT_SUCCESS(status)) { DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED); return TRUE; }
        break;
    case PnStepSuspend:
        AvdtpSuspend(&D->Avdtp);
        return TRUE;
    case PnStepNotifyState:
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
        return TRUE;
    case PnStepWaitRemoteOpen:
        /* T3/T9: the receiver configured the stream; its OPEN normally follows and queues
         * the next pass, the planned retry covers a receiver that waits for ours */
        return FALSE;
    case PnStepArmSuspendTimer: {
        /* T2: the receiver started while we are paused or stopped: SUSPEND after the usual delay */
        LARGE_INTEGER due;
        due.QuadPart = -PN_HNS_PER_MS * 3000;
        KeSetTimer(&D->SuspendTimer, due, &D->SuspendDpc);
        return TRUE;
    }
    case PnStepAbortStream: {
        /* T13, C4: media writes stopped completing. A receiver that answers a probe once
         * the writes flow again was behind a radio fade: keep the stream, as HEAD did
         * (review round 3 #6). The pump keeps running meanwhile. */
        NTSTATUS probe = AvdtpProbe(&D->Avdtp);
        BOOLEAN stalled = PumpMediaStalled(&D->Pump, PN_MEDIA_STALL_MS);
        for (ULONG waited = 0; stalled && PnStallJackEvent(probe) == PnJackStallAnswered && waited < PN_STALL_PROBE_SETTLE_MS; waited += 50) {
            LARGE_INTEGER wait;
            wait.QuadPart = -PN_HNS_PER_MS * 50;
            KeDelayExecutionThread(KernelMode, FALSE, &wait);   /* the answer may overtake the write completions */
            stalled = PumpMediaStalled(&D->Pump, PN_MEDIA_STALL_MS);
        }
        PN_STALL_ACTION act = PnStallProbeAction(probe, stalled, D->Avdtp.ResetPending != 0);
        if (act == PnStallKeep) {
            PN_INFO("media writes resumed and the receiver answers (0x%08x); stream kept", probe);
            return TRUE;
        }
        if (act != PnStallYield)
            PN_WARN("media transport stalled (probe 0x%08x); %s", probe, act == PnStallAbort ? "aborting the stream" : "dropping the channels");
        StopPump(D);
        if (act == PnStallAbort) {
            status = AvdtpAbort(&D->Avdtp);
            if (PnStallSessionReplaced(status, D->Avdtp.ResetPending != 0)) act = PnStallYield;
            else if (!NT_SUCCESS(status)) AvdtpDisconnect(&D->Avdtp);
        } else if (act == PnStallDisconnect) {
            status = probe;   /* unanswered: an ABORT could not be answered either */
            AvdtpDisconnect(&D->Avdtp);
        }
        if (act == PnStallYield) {
            /* review round 5 #1: the receiver opened a new signalling session meanwhile;
             * its reset ends the old stream and queues the next pass */
            PN_INFO("media transport stalled; the receiver reconnected signalling, stream reset follows");
            DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
            return TRUE;
        }
        D->ReceiverUnreachable = TRUE;
        /* C6 (review round 5 #5: the jack stays plugged through an outage) */
        JackEvent(D, PnStallJackEvent(status));
        SetActiveConfig(D, NULL);
        PnSetupRetryObserve(&D->SetupRetry, NowMs(), 0);
        PnSetupRetryFailed(&D->SetupRetry);
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
        return TRUE;
    }
    case PnStepLinkLost:
        /* T13: ACL down for our address: every channel is gone */
        PN_WARN("ACL to the receiver lost; closing the session");
        StopPump(D);
        D->ReceiverUnreachable = TRUE;
        AvdtpDisconnect(&D->Avdtp);
        SetActiveConfig(D, NULL);
        PnSetupRetryObserve(&D->SetupRetry, NowMs(), 0);
        PnSetupRetryFailed(&D->SetupRetry);
        DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
        return TRUE;
    default:
        return TRUE;
    }
    if (NT_SUCCESS(status)) return TRUE;
    ReconcileFailed(D, Pass, Step, status);
    return FALSE;
}

/* Make reality match AudioRequest: snapshot, plan (transport_policy.h), execute. A pass
 * that ends with audio wanted but not streaming always leaves exactly one retry armed
 * (T4): the plan's, the failure policy's, or the plain setup back-off. */
static VOID Reconcile(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN SuspendDue)
{
    PAGED_CODE();
    PN_RECONCILE_INPUT in;
    PN_RECONCILE_PLAN plan;
    PN_RECONCILE_PASS pass;
    RtlZeroMemory(&pass, sizeof(pass));
    ApplyIntents(D, &pass);
    ReconcileSnapshot(D, SuspendDue, &in);
    PnPlanReconcile(&in, &plan);
    for (ULONG i = 0; i < plan.Count; i++) {
        if (!ReconcileStep(D, &pass, (PN_RECONCILE_STEP)plan.Steps[i])) break;
    }
    if (plan.RetryDelayMs && !pass.RetryArmed) ArmRetry(D, &pass, plan.RetryDelayMs);
    BOOLEAN streaming = PumpIsStreaming(&D->Pump) && AvdtpGetState(&D->Avdtp) == AvdtpStateStreaming;
    if (streaming) {
        PnSetupRetryObserve(&D->SetupRetry, NowMs(), 1);
        D->ConsecutiveTimeouts = 0;
        D->ReceiverUnreachable = FALSE;
        InterlockedExchange(&D->LocalStartRequested, 0);
    } else if (PnReconcileWantsAudio(&in) && !pass.RetryArmed && !D->Removing) {
        ArmRetry(D, &pass, PnSetupRetryDelayMs(D->SetupRetry.failures));
    }
    NoteNegotiatedFormat(D);
}

/* ------------------------------------------------------------------------- */
/* Work items / timers                                                          */
/* ------------------------------------------------------------------------- */

static IO_WORKITEM_ROUTINE ConnectWorker;
static VOID ConnectWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (!D) return;
    InterlockedExchange(&D->ConnectQueued, 0);
    QuerySdpOnce(D);   /* T5: also for receiver-initiated sessions, without StateMutex */
    PnMutexAcquire(&D->StateMutex);
    if (!D->Removing && !D->PowerSuspended) {
        // Clear any pre-suspend session before idle capability discovery too.
        Reconcile(D, FALSE);
        /* Opportunistic connect: bring the signalling channel up and learn capabilities so
         * the endpoint's format list matches the sink before audio starts. When audio is
         * already wanted, Reconcile does the same with retry scheduling; connecting here too
         * would double the time a speaker that is off holds StateMutex. */
        BOOLEAN want = D->AudioRequest == PnAudioRun && D->AudioStreamOpen;
        /* C7: not after a user disconnect */
        if (!want && !D->PowerSuspended && !D->UserDisconnected && NT_SUCCESS(EnsureSignalling(D))) {
            EnsureDiscovered(D);
            if (D->Config.Flags & PNA2DP_CFG_AUTO_START_ON_CONNECT) EnsureConfiguredAndOpen(D, NULL);
            NoteNegotiatedFormat(D);
        }
        /* review #11: session dropped after an unanswered stall while the ACL stays up: no
         * ACL-up event will come, so keep trying (over the live ACL: nothing is paged) */
        if (PnRetryUnreachable(want, D->UserDisconnected != 0, D->ReceiverUnreachable, D->Link.AclConnected, D->Avdtp.SignallingUp)) {
            ULONG delay = PnReconnectDelayMs(D->ReconnectAttempts, D->Config.ReconnectDelayMs);
            if (D->ReconnectAttempts < 10) D->ReconnectAttempts++;
            PN_INFO("receiver unreachable with the ACL up; next connect attempt in %u ms", delay);
            ScheduleReconnect(D, delay);
        }
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

static IO_WORKITEM_ROUTINE AudioWorker;
static VOID AudioWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Context;
    if (!D) return;
    LONG flags = InterlockedExchange(&D->AudioQueued, 0);
    QuerySdpOnce(D);   /* T5: also for receiver-initiated sessions, without StateMutex */
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

static KDEFERRED_ROUTINE SuspendDpc;
static VOID SuspendDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (D && D->AudioRequest != PnAudioRun) QueueAudioWork(D, TRUE);
}

static KDEFERRED_ROUTINE ReconnectDpc;
static VOID ReconnectDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Ctx, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc); UNREFERENCED_PARAMETER(A1); UNREFERENCED_PARAMETER(A2);
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (D) DeviceQueueConnect(D);
}

static IO_WORKITEM_ROUTINE ScoWorker;
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
    BOOLEAN signalling = FALSE;
    NTSTATUS status = AvdtpAcceptIncoming(&D->Avdtp, Params, &signalling);
    if (!NT_SUCCESS(status) && status != STATUS_REQUEST_NOT_ACCEPTED) PN_WARN("accept incoming failed 0x%08x", status);
    /* the receiver connected its signalling channel to us: it is there, and a user
     * disconnect no longer holds. Not for a refused channel or our stream's media channel,
     * which would cancel a disconnect still queued (review round 4 #1). */
    if (!PnIncomingLiftsUserDisconnect(signalling ? PnIncomingSignalling : PnIncomingMedia, NT_SUCCESS(status))) return;
    InterlockedExchange(&D->UserDisconnected, 0);
    JackEvent(D, PnJackSignallingUp);
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
    ULONG fx = PnAclEventAction(Connected, D->Avdtp.SignallingUp);
    JackEvent(D, Connected ? PnJackAclUp : PnJackAclDown);   /* C6 */
    /* the receiver may come back at another level (findings #3/#8): the user's volume applies
     * from the next stream start until Windows sends absolute volume again */
    if (!Connected) PnRemoteVolumeLinkLost(&D->ReceiverHoldsVolume);
    if (fx & PN_ACL_FX_RESET_BUDGET) {
        InterlockedExchange(&D->ResetReconnectBudget, 1);
        D->ReceiverUnreachable = FALSE;
    }
    /* give the headset a chance to initiate AVDTP itself before we do */
    if (fx & PN_ACL_FX_RECONNECT_SOON) ScheduleReconnect(D, 2500);
    /* T13: ACL down for our address: treat every channel as gone (in the worker) */
    if (fx & PN_ACL_FX_LINK_LOST) {
        InterlockedExchange(&D->LinkLostPending, 1);
        QueueAudioWork(D, FALSE);
    }
}

/* C1 / T11. The power callback never waits for Bluetooth I/O: the worker acts. */
VOID DevicePowerChanged(_Inout_ PPNA2DP_DEVICE D, _In_ DEVICE_POWER_STATE State, _In_ SYSTEM_POWER_STATE SystemState)
{
    if (!D || D->Removing) return;
    if (State != PowerDeviceD0) {
        ULONG fx = PnPowerOnDx(SystemState == PowerSystemWorking);
        if (fx & PN_PWR_BLOCK_WORK) InterlockedExchange(&D->PowerSuspended, 1);
        // Publish the required reset while still suspended; a worker must not
        // observe D0 before it knows that the old Bluetooth session is stale.
        if (fx & PN_PWR_RESET_ON_RESUME) InterlockedExchange(&D->PowerResumePending, 1);
        /* idle D3 in S0 / Modern Standby: stop the pump and SUSPEND, keep signalling */
        if (fx & PN_PWR_SUSPEND_STREAM) InterlockedExchange(&D->PowerIdle, 1);
        if (fx & PN_PWR_CANCEL_RECONNECT) KeCancelTimer(&D->ReconnectTimer);
        if (fx & PN_PWR_QUEUE_WORK) QueueAudioWork(D, (fx & PN_PWR_SUSPEND_STREAM) != 0);
    } else {
        BOOLEAN wasIdle = InterlockedExchange(&D->PowerIdle, 0) != 0;
        if (InterlockedExchange(&D->PowerSuspended, 0)) {
            InterlockedExchange(&D->ResetReconnectBudget, 1);
            DeviceQueueConnect(D);
        } else if (wasIdle) {
            QueueAudioWork(D, FALSE);
        }
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
    D->ActiveStreamed = FALSE;
    KeReleaseSpinLock(&D->StatusLock, irql);
    /* T3/T9/T10: the receiver drives this stream's setup; the worker gives its OPEN time */
    D->RemoteConfiguredHns = PnNowHns();
    D->RemoteConfigured = TRUE;
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
    D->ActiveStreamed = FALSE;
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
        D->ActiveStreamed = FALSE;
        KeReleaseSpinLock(&D->StatusLock, irql);
        D->RemoteConfigured = FALSE;
        InterlockedExchange(&D->RemoteSuspended, 0);
    }
    if (Remote) QueueAudioWork(D, FALSE);   /* reconcile pump with the remote's decision */
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
}

/* T2: the receiver STARTed or SUSPENDed the stream (before the state change is applied,
 * so the pass it queues sees this). A SUSPEND is respected until a new local playback
 * start, a receiver START or the back-off (PN_REMOTE_SUSPEND_BACKOFF_MS). */
static VOID OnRemoteStreamControl(_In_ PVOID Ctx, _In_ BOOLEAN Started)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    if (Started) {
        InterlockedExchange(&D->RemoteSuspended, 0);
        InterlockedExchange(&D->RemoteSuspendRetried, 0);
        InterlockedExchange(&D->RemoteStartedPending, 1);
    } else {
        PN_INFO("receiver suspended the stream");
        D->RemoteSuspendHns = PnNowHns();
        if (!PnRemoteSuspendKeepsRetried(D->RemoteSuspendRetried != 0, AgeMs(D->RemoteSuspendRetryHns)))
            InterlockedExchange(&D->RemoteSuspendRetried, 0);
        InterlockedExchange(&D->LocalStartRequested, 0);
        InterlockedExchange(&D->RemoteSuspended, 1);
    }
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
    StopPump(D);
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
    StopPump(D);
    PnMutexRelease(&D->StateMutex);
    QueueAudioWork(D, FALSE);
}

VOID DeviceMediaWriteDone(PVOID Ctx, NTSTATUS Status, ULONG Bytes, LONGLONG SubmitTimeHns)
{
    PPNA2DP_DEVICE D = (PPNA2DP_DEVICE)Ctx;
    PumpOnWriteDone(&D->Pump, Status, Bytes, SubmitTimeHns);
}

/* C4 / T13: media writes are outstanding and none completed for a while (the pump's
 * PumpMediaStalled). The worker probes the receiver: a fade that has passed keeps the
 * stream; otherwise it stops the pump, ABORTs the stream (or drops the channels) and
 * retries with the setup back-off. <= DISPATCH. */
VOID DeviceMediaStalled(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Removing) return;
    if (InterlockedExchange(&D->MediaStallPending, 1) == 0) PN_WARN("media writes stalled");
    QueueAudioWork(D, FALSE);
}

/* ------------------------------------------------------------------------- */
/* Audio endpoint -> device                                                     */
/* ------------------------------------------------------------------------- */

/* T8: the audio-engine threads below take only IntentMutex; the worker does the
 * Bluetooth side (connect / configure on open, SUSPEND on close). */
NTSTATUS DeviceAudioStreamOpened(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG Rate, _In_ ULONG Channels, _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat)
{
    PAGED_CODE();
    PnMutexAcquire(&D->IntentMutex);
    if (D->Removing || D->TornDown) { PnMutexRelease(&D->IntentMutex); return STATUS_DEVICE_REMOVED; }
    NTSTATUS status = PumpConfigureInput(&D->Pump, Rate, Channels, ContainerBits, ValidBits, IsFloat);
    if (!NT_SUCCESS(status)) { PnMutexRelease(&D->IntentMutex); return status; }
    D->AudioRate = Rate;
    D->AudioChannels = Channels;
    D->AudioBits = ContainerBits;
    D->AudioValidBits = ValidBits;
    D->AudioFloat = IsFloat;
    D->AudioStreamOpen = TRUE;
    D->IntentGen++;
    PnMutexRelease(&D->IntentMutex);
    DeviceQueueConnect(D);
    return STATUS_SUCCESS;
}

VOID DeviceAudioStreamClosed(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    KeCancelTimer(&D->SuspendTimer);
    InterlockedExchange64(&D->RenderHoldUntilHns, 0);
    PnMutexAcquire(&D->IntentMutex);
    D->AudioRequest = PnAudioStop;
    D->AudioStreamOpen = FALSE;
    D->IntentGen++;
    PumpStopStream(&D->Pump);
    PumpReleaseInput(&D->Pump);
    PnMutexRelease(&D->IntentMutex);
    /* the worker SUSPENDs a streaming stream (no Bluetooth wait here); nothing after DeviceStop */
    QueueAudioWork(D, TRUE);
    DeviceNotifyEvent(D, PNA2DP_EVENT_STATE_CHANGED);
}

VOID DeviceAudioStateChanged(_Inout_ PPNA2DP_DEVICE D, _In_ PN_AUDIO_REQUEST Request)
{
    PN_AUDIO_REQUEST old = D->AudioRequest;
    /* T2: a new local playback start may START a stream the receiver suspended */
    if (Request == PnAudioRun && old != PnAudioRun) {
        InterlockedExchange(&D->LocalStartRequested, 1);
        InterlockedExchange(&D->RemoteSuspendRetried, 0);
        InterlockedExchange(&D->UserDisconnected, 0);   /* C7: a new playback start connects again */
    } else if (Request != PnAudioRun) InterlockedExchange(&D->LocalStartRequested, 0);
    D->AudioRequest = Request;
    if (D->Removing) return;   /* a stream that outlives a surprise removal: nothing to drive */
    if (Request == PnAudioRun) {
        /* the stream must start first: hold the render clock rather than render audio the
         * start would discard (wave_math.h, findings #23/#24) */
        if (old != PnAudioRun && !PumpIsStreaming(&D->Pump))
            InterlockedExchange64(&D->RenderHoldUntilHns, PnNowHns() + (LONGLONG)PN_WAVE_START_HOLD_MS * PN_HNS_PER_MS);
        KeCancelTimer(&D->SuspendTimer);
        QueueAudioWork(D, FALSE);
    } else {
        InterlockedExchange64(&D->RenderHoldUntilHns, 0);
        /* the engine stopped feeding the ring: the WaveRT stream already ended the sound with a
         * fade and drained the encoder (PumpFinishStream with its held-back frames, #15/#26, #1/#7) */
        LARGE_INTEGER due;
        due.QuadPart = -PN_HNS_PER_MS * 3000;      /* SUSPEND after 3 s of silence */
        KeSetTimer(&D->SuspendTimer, due, &D->SuspendDpc);
        QueueAudioWork(D, FALSE);
    }
}

/* T2 (review round 4 #7): a pause on the receiver (SUSPEND, and the player pauses) often
 * leaves the endpoint in RUN rendering silence, so no RUN edge follows when play resumes.
 * Audio after at least PN_RESUME_SILENCE_MS of silence while the receiver keeps our stream
 * suspended is that playback start; audio that never paused (a multipoint sink serving
 * another source) is not. */
BOOLEAN DeviceAwaitsAudioResume(_In_ PPNA2DP_DEVICE D)
{
    return D->RemoteSuspended && !D->LocalStartRequested;
}

BOOLEAN DeviceRenderHold(_In_ PPNA2DP_DEVICE D, _In_ LONGLONG NowHns)
{
    LONGLONG until = InterlockedCompareExchange64(&D->RenderHoldUntilHns, 0, 0);
    if (!until) return FALSE;
    return PnWaveStartHold(NowHns, until, PumpIsStreaming(&D->Pump));
}

VOID DeviceNoteRenderedAudio(_Inout_ PPNA2DP_DEVICE D, _In_ BOOLEAN Watching, _In_ BOOLEAN Audible, _In_ ULONG ElapsedUs)
{
    if (!PnResumeDetect(&D->ResumeDetect, Watching, !Audible, ElapsedUs)) return;
    if (!DeviceAwaitsAudioResume(D) || D->Removing) return;
    PN_INFO("audio resumed while the receiver suspended the stream: starting it");
    if (!PumpIsStreaming(&D->Pump))   /* the rest of the sound waits for the start (#24) */
        InterlockedExchange64(&D->RenderHoldUntilHns, PnNowHns() + (LONGLONG)PN_WAVE_START_HOLD_MS * PN_HNS_PER_MS);
    InterlockedExchange(&D->LocalStartRequested, 1);
    InterlockedExchange(&D->RemoteSuspendRetried, 0);
    QueueAudioWork(D, FALSE);
}

VOID DeviceAudioVolumeChanged(_Inout_ PPNA2DP_DEVICE D, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted)
{
    D->VolumeDb16 = VolumeDb16;
    D->Muted = Muted;
    KIRQL irql;
    KeAcquireSpinLock(&D->StatusLock, &irql);
    D->Config.VolumeDb16 = VolumeDb16;
    KeReleaseSpinLock(&D->StatusLock, irql);
    PumpSetVolume(&D->Pump, SoftwareVolumeDb16(D, VolumeDb16), Muted);
}

/* ------------------------------------------------------------------------- */
/* Control interface -> device                                                  */
/* ------------------------------------------------------------------------- */

NTSTATUS DeviceSetConfig(_Inout_ PPNA2DP_DEVICE D, _In_ const PNA2DP_CONFIG *Cfg)
{
    PAGED_CODE();
    PNA2DP_CONFIG c = *Cfg;
    if (!RegCfgValidate(&c)) return STATUS_INVALID_PARAMETER;
    /* T8: IntentMutex only; a renegotiation (CLOSE) is the worker's */
    BOOLEAN renegotiate = FALSE;
    PnMutexAcquire(&D->IntentMutex);
    PNA2DP_CONFIG old = D->Config;
    NTSTATUS saved = RegCfgSaveDevice(D->DriverObject, D->Link.RemoteAddress, &c, D->Link.RemoteName);
    if (!NT_SUCCESS(saved)) { PnMutexRelease(&D->IntentMutex); return saved; }
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
                           ((old.Flags ^ c.Flags) & (PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_DELAY_REPORTING |
                                                     PNA2DP_CFG_SBC_XQ));
    /* C6: the endpoint offers formats of the enabled codecs (and all their rates on request) */
    BOOLEAN formatsChanged = old.EnabledCodecMask != c.EnabledCodecMask || ((old.Flags ^ c.Flags) & PNA2DP_CFG_EXPOSE_ALL_RATES);
    PumpSetAbr(&D->Pump, (c.Flags & PNA2DP_CFG_ABR_ENABLE) != 0 &&
        (D->Pump.Cfg.Codec != PNA2DP_CODEC_LDAC || c.Ldac.AbrEnable != 0));
    D->Pump.TargetLatencyMs = c.TargetLatencyMs;
    D->Pump.MaxLatencyMs = c.MaxLatencyMs;
    RefreshLocalSepAvailability(D);

    if (codecChanged && D->ActiveValid) {
        /* Live path: if only the SBC bitpool changed, apply in band; otherwise renegotiate. */
        PNA2DP_CONFIG bitpoolOnly = old;
        bitpoolOnly.Sbc.MaxBitpool = c.Sbc.MaxBitpool;
        BOOLEAN onlyBitpool = D->Active.Codec == PNA2DP_CODEC_SBC && old.Sbc.MaxBitpool != c.Sbc.MaxBitpool &&
                              c.Sbc.MaxBitpool >= D->Active.SbcMinBitpool && c.Sbc.MaxBitpool <= D->Active.SbcMaxBitpool &&
                              RtlCompareMemory(&bitpoolOnly, &c, sizeof(c)) == sizeof(c);
        if (onlyBitpool) {
            PumpSetSbcBitpool(&D->Pump, c.Sbc.MaxBitpool);
            KeAcquireSpinLock(&D->StatusLock, &irql);
            D->Active.SbcBitpool = c.Sbc.MaxBitpool;
            KeReleaseSpinLock(&D->StatusLock, irql);
        } else if (c.Flags & PNA2DP_CFG_APPLY_LIVE) {
            /* stop the pump now; the worker CLOSEs and configures again (ApplyIntents) */
            PumpStopStream(&D->Pump);
            D->IntentGen++;
            InterlockedExchange(&D->RenegotiatePending, 1);
            renegotiate = TRUE;
        }
    }
    PnMutexRelease(&D->IntentMutex);
    if (renegotiate) QueueAudioWork(D, FALSE);
    if (formatsChanged) PnAudioFormatsChanged(D);
    DeviceNotifyEvent(D, PNA2DP_EVENT_CONFIG_CHANGED);
    return STATUS_SUCCESS;
}

/* T8: the IOCTL thread stops the pump and records the request; the connect worker drops
 * the session, resets the back-off and connects again (ApplyIntents). */
NTSTATUS DeviceReconnect(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    PnMutexAcquire(&D->IntentMutex);
    PumpStopStream(&D->Pump);
    D->IntentGen++;
    InterlockedExchange(&D->UserDisconnectPending, 0);   /* the latest request wins */
    InterlockedExchange(&D->UserDisconnected, 0);
    InterlockedExchange(&D->UserResetPending, 1);
    PnMutexRelease(&D->IntentMutex);
    DeviceQueueConnect(D);
    return STATUS_SUCCESS;
}

/* KSPROPERTY_ONESHOT_RECONNECT asks for a connection, not a reset (review #12): it lifts a
 * user disconnect, starts the back-off over and queues the connect worker, whose
 * EnsureSignalling / Reconcile do nothing to a session that is already up. A working
 * stream keeps playing; IOCTL_PNA2DP_RECONNECT (DeviceReconnect) stays the reset. */
NTSTATUS DeviceRequestConnect(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    PnMutexAcquire(&D->IntentMutex);
    InterlockedExchange(&D->UserDisconnectPending, 0);   /* the latest request wins */
    InterlockedExchange(&D->UserDisconnected, 0);
    InterlockedExchange(&D->UserConnectPending, 1);
    PnMutexRelease(&D->IntentMutex);
    DeviceQueueConnect(D);
    return STATUS_SUCCESS;
}

/* C7 (KSPROPERTY_ONESHOT_DISCONNECT), T8 pattern: the property thread stops the pump and
 * records the request; the worker drops the session, unplugs the jack and connects
 * nothing until ONESHOT_RECONNECT, a new local playback start or an accepted incoming
 * signalling channel from the receiver (ApplyIntents, PnPlanReconcile). */
NTSTATUS DeviceRequestDisconnect(_Inout_ PPNA2DP_DEVICE D)
{
    PAGED_CODE();
    PnMutexAcquire(&D->IntentMutex);
    PumpStopStream(&D->Pump);
    D->IntentGen++;
    InterlockedExchange(&D->UserResetPending, 0);   /* the latest request wins */
    InterlockedExchange(&D->UserDisconnected, 1);
    InterlockedExchange(&D->UserDisconnectPending, 1);
    PnMutexRelease(&D->IntentMutex);
    QueueAudioWork(D, FALSE);
    return STATUS_SUCCESS;
}

VOID DeviceHostChanged(_Inout_ PPNA2DP_DEVICE D)
{
    if (D->Removing) return;
    RefreshLocalSepAvailability(D);   /* T10: what DISCOVER advertises */
    PnAudioFormatsChanged(D);         /* C6: no-op unless the table changed */
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
    ExInitializeFastMutex(&D->IntentMutex);
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
    if (!NT_SUCCESS(RtlStringCbLengthW(D->Link.RemoteNameW, sizeof(D->Link.RemoteNameW), &nameBytes))) nameBytes = 0;
    NTSTATUS nameStatus = IoSetDevicePropertyData(D->Pdo, &DEVPKEY_Device_FriendlyName,
        LOCALE_NEUTRAL, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING,
        (ULONG)(nameBytes + sizeof(WCHAR)), D->Link.RemoteNameW);
    if (!NT_SUCCESS(nameStatus)) PN_WARN("device friendly name failed 0x%08x", nameStatus);
    NTSTATUS loaded = RegCfgLoadDevice(D->DriverObject, D->Link.RemoteAddress, &D->Config);
    if (!NT_SUCCESS(loaded)) {
        RegCfgDefaults(&D->Config);
        /* only where nothing is stored: a saved configuration this build cannot read (another
         * ABI, a rejected value, an I/O error) is never overwritten; SET_CONFIG saves it */
        if (PnConfigShouldSaveDefaults(loaded)) RegCfgSaveDevice(D->DriverObject, D->Link.RemoteAddress, &D->Config, D->Link.RemoteName);
        else PN_WARN("saved configuration not loaded (0x%08x): defaults in memory, the stored value is kept", loaded);
    }
    D->VolumeDb16 = D->Config.VolumeDb16;
    D->WorkConfig = D->Config;   /* each worker pass takes a fresh copy (ApplyIntents) */

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
    cb.OnRemoteStreamControl = OnRemoteStreamControl;
    status = AvdtpInitialize(&D->Avdtp, &D->Link, D->Fdo, &cb);
    if (!NT_SUCCESS(status)) return status;
    BuildLocalSeps(D);
    RefreshLocalSepAvailability(D);

    status = PnLinkRegisterServer(&D->Link, OnIncoming, D);
    if (!NT_SUCCESS(status)) PN_WARN("incoming AVDTP connections unavailable (0x%08x)", status);
    PnLinkRegisterRadioEvents(&D->Link, OnSco, OnAcl, D);
    status = ControlRegisterInterface(D);
    if (!NT_SUCCESS(status)) return status;

    KIRQL irql;
    KeAcquireSpinLock(&g_DeviceListLock, &irql);
    InsertTailList(&g_DeviceList, &D->GlobalLink);
    KeReleaseSpinLock(&g_DeviceListLock, irql);

    PumpSetVolume(&D->Pump, SoftwareVolumeDb16(D, D->VolumeDb16), FALSE);
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
    StopPump(D);
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
    PnMutexAcquire(&D->IntentMutex);   /* DeviceAudioStreamOpened checks it under IntentMutex */
    D->TornDown = TRUE;
    PnMutexRelease(&D->IntentMutex);
    PnMutexRelease(&D->StateMutex);
    if (D->Pump.Device) PumpUninitialize(&D->Pump);
    if (D->Avdtp.Link) AvdtpUninitialize(&D->Avdtp);
    PnLinkUninitialize(&D->Link);
    if (D->ConnectWorkItem) { IoFreeWorkItem(D->ConnectWorkItem); D->ConnectWorkItem = NULL; }
    if (D->AudioWorkItem) { IoFreeWorkItem(D->AudioWorkItem); D->AudioWorkItem = NULL; }
    if (D->ScoWorkItem) { IoFreeWorkItem(D->ScoWorkItem); D->ScoWorkItem = NULL; }
}
