/*
 * avdtp.cpp - AVDTP 1.3 signalling (initiator and acceptor) over PN L2CAP channels.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"
#include "transport_policy.h"

#define AVDTP_EVT_SIG_DOWN     0x1
#define AVDTP_EVT_MEDIA_DOWN   0x2
#define AVDTP_EVT_MEDIA_UP     0x4
#define AVDTP_EVT_SIG_UP       0x8
#define AVDTP_EVT_RUNNING      0x40000000   /* event work item queued or running */

static VOID AvdtpQueueEvent(_Inout_ PAVDTP_SESSION S, _In_ ULONG Bits);
static VOID AvdtpAcpWorker(_In_ PAVDTP_SESSION S);

/* ------------------------------------------------------------------------- */
/* Helpers                                                                      */
/* ------------------------------------------------------------------------- */

static const char *SigName(UCHAR id)
{
    switch (id) {
    case AVDTP_DISCOVER: return "DISCOVER";
    case AVDTP_GET_CAPABILITIES: return "GET_CAPABILITIES";
    case AVDTP_SET_CONFIGURATION: return "SET_CONFIGURATION";
    case AVDTP_GET_CONFIGURATION: return "GET_CONFIGURATION";
    case AVDTP_RECONFIGURE: return "RECONFIGURE";
    case AVDTP_OPEN: return "OPEN";
    case AVDTP_START: return "START";
    case AVDTP_CLOSE: return "CLOSE";
    case AVDTP_SUSPEND: return "SUSPEND";
    case AVDTP_ABORT: return "ABORT";
    case AVDTP_SECURITY_CONTROL: return "SECURITY_CONTROL";
    case AVDTP_GET_ALL_CAPABILITIES: return "GET_ALL_CAPABILITIES";
    case AVDTP_DELAYREPORT: return "DELAYREPORT";
    default: return "?";
    }
}

/* T1: the receiver's SEP of a stream that ended is free again, whatever DISCOVER said. */
static VOID AvdtpReleaseRemoteSep(_Inout_ PAVDTP_SESSION S, _In_ UCHAR Seid)
{
    for (ULONG i = 0; i < S->RemoteSepCount && i < AVDTP_MAX_SEPS; i++) if (S->RemoteSeps[i].Seid == Seid) S->RemoteSeps[i].InUse = 0;
}

/* StreamMutex held. */
static VOID SetStreamState(_Inout_ PAVDTP_SESSION S, _In_ AVDTP_STREAM_STATE New, _In_ BOOLEAN Remote)
{
    AVDTP_STREAM_STATE old;
    KIRQL irql;
    KeAcquireSpinLock(&S->Lock, &irql);
    old = S->StreamState;
    S->StreamState = New;
    if (New == AvdtpStateIdle) {
        /* the stream is gone: its delay report and negotiation no longer apply (T6) */
        S->SinkDelayTenthsMs = 0;
        S->DelayReportingNegotiated = FALSE;
    }
    KeReleaseSpinLock(&S->Lock, irql);
    if (New == AvdtpStateIdle && old != AvdtpStateIdle) AvdtpReleaseRemoteSep(S, S->RemoteSeid);
    if (old != New) {
        PN_INFO("AVDTP stream state %u -> %u (%s)", old, New, Remote ? "remote" : "local");
        if (S->Cb.OnStreamStateChanged) S->Cb.OnStreamStateChanged(S->Cb.Context, old, New, Remote);
    }
}

AVDTP_STREAM_STATE AvdtpGetState(_In_ PAVDTP_SESSION S) { return S->StreamState; }
PPN_L2CAP_CHANNEL AvdtpMediaChannel(_In_ PAVDTP_SESSION S) { return &S->Media; }

/* Media MTU limits from the local radio (recovery_policy.h, T12); no link: no cap. */
static USHORT AvdtpMediaMtuCap(_In_ PAVDTP_SESSION S)
{
    PPN_BTH_LINK l = S->Link;
    return l ? PnPumpMediaMtuCap(l->RadioInfoValid, l->RadioInfo.radioInfo.lmpSupportedFeatures) : (USHORT)PN_MEDIA_MTU_NO_CAP;
}

static USHORT AvdtpPreferredMediaMtu(_In_ PAVDTP_SESSION S)
{
    PPN_BTH_LINK l = S->Link;
    return l ? PnPreferredMediaMtu(l->RadioInfoValid, l->RadioInfo.radioInfo.lmpSupportedFeatures) : (USHORT)PN_L2CAP_MEDIA_MTU_PREF;
}

USHORT AvdtpMediaMtu(_In_ PAVDTP_SESSION S) { return PnEffectiveMediaMtu(S->Media.OutMtu, AvdtpMediaMtuCap(S), PN_L2CAP_SIG_MTU); }

/* Send one signalling message, fragmenting if it exceeds the channel MTU. */
static NTSTATUS AvdtpWriteMessage(_Inout_ PAVDTP_SESSION S, _In_ UCHAR MsgType, _In_ UCHAR Label, _In_ UCHAR SignalId,
                                  _In_reads_bytes_opt_(Len) const UCHAR *Payload, _In_ ULONG Len)
{
    PPN_L2CAP_CHANNEL ch = &S->Signalling;
    if (Len > AVDTP_MAX_SIGNAL_PAYLOAD || (!Payload && Len)) return STATUS_INVALID_PARAMETER;
    ULONG mtu = PN_MIN(ch->OutMtu, ch->WriteCapacity);
    if (mtu < 8) return STATUS_INVALID_PARAMETER;
    if (!PnChannelIsConnected(ch)) return STATUS_CONNECTION_DISCONNECTED;

    if (Len + 2 <= mtu) {
        PPN_L2CAP_WRITE w = PnChannelAcquireWrite(ch);
        if (!w) return STATUS_DEVICE_BUSY;
        w->Buffer[0] = (UCHAR)((Label << 4) | (AVDTP_PKT_SINGLE << 2) | MsgType);
        w->Buffer[1] = (UCHAR)(SignalId & 0x3F);
        if (Len) RtlCopyMemory(w->Buffer + 2, Payload, Len);
        w->Length = Len + 2;
        return PnChannelSubmitWrite(ch, w);
    }

    /* Fragmented: START carries NOSP, CONTINUE/END carry only the 1-byte header */
    ULONG firstData = mtu - 3;
    ULONG restData = mtu - 1;
    ULONG nosp = 1 + (Len - firstData + restData - 1) / restData;
    if (nosp > 255) return STATUS_BUFFER_OVERFLOW;
    ULONG offset = 0;
    for (ULONG i = 0; i < nosp; i++) {
        PPN_L2CAP_WRITE w = PnChannelAcquireWrite(ch);
        if (!w) return STATUS_DEVICE_BUSY;
        UCHAR pkt = (i == 0) ? AVDTP_PKT_START : (i == nosp - 1) ? AVDTP_PKT_END : AVDTP_PKT_CONTINUE;
        ULONG hdr = (i == 0) ? 3 : 1;
        ULONG chunk = PN_MIN(Len - offset, mtu - hdr);
        w->Buffer[0] = (UCHAR)((Label << 4) | (pkt << 2) | MsgType);
        if (i == 0) { w->Buffer[1] = (UCHAR)nosp; w->Buffer[2] = (UCHAR)(SignalId & 0x3F); }
        RtlCopyMemory(w->Buffer + hdr, Payload + offset, chunk);
        w->Length = hdr + chunk;
        NTSTATUS status = PnChannelSubmitWrite(ch, w);
        if (!NT_SUCCESS(status)) return status;
        offset += chunk;
    }
    return STATUS_SUCCESS;
}

static NTSTATUS AvdtpSendResponse(_Inout_ PAVDTP_SESSION S, _In_ UCHAR Label, _In_ UCHAR SignalId, _In_ BOOLEAN Accept,
                                  _In_reads_bytes_opt_(Len) const UCHAR *Payload, _In_ ULONG Len)
{
    return AvdtpWriteMessage(S, Accept ? AVDTP_MSG_RESPONSE_ACCEPT : AVDTP_MSG_RESPONSE_REJECT, Label, SignalId, Payload, Len);
}

/* The receiver's commands that arrived before our answer are finished before the answer
 * is judged (PnIntMustWaitForAcp): e.g. its OPEN crossing ours must be refused as the
 * collision while IntSignal is still OPEN, not accepted after AvdtpTxnEnd. PASSIVE, without
 * StreamMutex (the acceptor takes it); bounded at 1 s like the opposite wait. */
static VOID AvdtpWaitAcpBefore(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    KIRQL irql;
    KeAcquireSpinLock(&S->Lock, &irql);
    BOOLEAN answered = S->Txn.MessageType != 0xFF;
    ULONG before = S->Txn.AcpBefore;
    KeReleaseSpinLock(&S->Lock, irql);
    if (!answered) return;   /* woken by a reset or a lost channel: nothing to order against */
    for (ULONG i = 0; i < 40; i++) {
        KeClearEvent(&S->AcpProgress);   /* before the check: an advance after it sets the event */
        if (!PnIntMustWaitForAcp(before, (ULONG)InterlockedCompareExchange(&S->AcpDoneSeq, 0, 0))) return;
        LARGE_INTEGER t;
        t.QuadPart = -PN_HNS_PER_MS * 25;
        KeWaitForSingleObject(&S->AcpProgress, Executive, KernelMode, FALSE, &t);
    }
    PN_WARN("AVDTP: receiver commands before the answer still pending; judging it anyway");
}

/* INT: send a command and wait for the matching response. PASSIVE, under S->Mutex and
 * without StreamMutex, so the acceptor keeps answering the receiver meanwhile (T9).
 * Returns holding StreamMutex in every case: the caller judges the answer against the
 * stream as it is now (PnIntOutcome), applies it and calls AvdtpTxnEnd. */
static NTSTATUS AvdtpTransact(_Inout_ PAVDTP_SESSION S, _In_ UCHAR SignalId, _In_reads_bytes_opt_(Len) const UCHAR *Payload,
                              _In_ ULONG Len, _Out_opt_ PUCHAR ErrorCode)
{
    KIRQL irql;
    if (ErrorCode) *ErrorCode = 0;
    NTSTATUS pre = (NTSTATUS)PnTransactPrecheck(S->SignallingUp, S->ResetPending != 0);
    if (!NT_SUCCESS(pre)) { PnMutexAcquire(&S->StreamMutex); return pre; }
    if (S->Signalling.State == PnChannelConnecting) {
        /* the remote opened signalling and our accept has not completed yet */
        LARGE_INTEGER t;
        t.QuadPart = -PN_HNS_PER_MS * 5000;
        KeWaitForSingleObject(&S->Signalling.ConnectedEvent, Executive, KernelMode, FALSE, &t);
    }

    KeAcquireSpinLock(&S->Lock, &irql);
    /* Checked under Lock with the KeClearEvent below: a reset flagged after this point
     * wakes the wait (AvdtpResetStream) instead of being erased by the clear (review #1). */
    pre = (NTSTATUS)PnTransactPrecheck(S->SignallingUp, S->ResetPending != 0);
    if (!NT_SUCCESS(pre)) {
        KeReleaseSpinLock(&S->Lock, irql);
        PN_WARN("AVDTP %s not sent: %s", SigName(SignalId), pre == STATUS_CANCELLED ? "a new signalling session resets the stream" : "signalling down");
        PnMutexAcquire(&S->StreamMutex);
        return pre;
    }
    S->NextLabel = (UCHAR)((S->NextLabel + 1) & 0x0F);
    S->Txn.Label = S->NextLabel;
    S->Txn.SignalId = SignalId;
    S->Txn.Active = TRUE;
    S->Txn.PayloadLength = 0;
    S->Txn.MessageType = 0xFF;
    S->Txn.ErrorCode = 0;
    KeClearEvent(&S->Txn.Done);
    KeReleaseSpinLock(&S->Lock, irql);

    PN_VERB("AVDTP -> %s label %u len %u", SigName(SignalId), S->Txn.Label, Len);
    NTSTATUS status = AvdtpWriteMessage(S, AVDTP_MSG_COMMAND, S->Txn.Label, SignalId, Payload, Len);
    if (!NT_SUCCESS(status)) {
        KeAcquireSpinLock(&S->Lock, &irql);
        S->Txn.Active = FALSE;
        KeReleaseSpinLock(&S->Lock, irql);
        PnMutexAcquire(&S->StreamMutex);
        return status;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -PN_HNS_PER_MS * AVDTP_RESPONSE_TIMEOUT_MS;
    status = KeWaitForSingleObject(&S->Txn.Done, Executive, KernelMode, FALSE, &timeout);
    BOOLEAN timedOut = status == STATUS_TIMEOUT;
    if (!timedOut) AvdtpWaitAcpBefore(S);
    /* before the answer is judged: the receiver's commands behind it wait for AvdtpTxnEnd */
    PnMutexAcquire(&S->StreamMutex);
    KeAcquireSpinLock(&S->Lock, &irql);
    S->Txn.Active = FALSE;
    UCHAR type = S->Txn.MessageType;
    UCHAR err = S->Txn.ErrorCode;
    KeReleaseSpinLock(&S->Lock, irql);
    status = (NTSTATUS)PnTransactStatus(timedOut, S->SignallingUp, type, err);
    if (timedOut) {
        PN_WARN("AVDTP %s timed out", SigName(SignalId));
    } else if (status == STATUS_CANCELLED) {
        PN_WARN("AVDTP %s: no answer, a new signalling session took over", SigName(SignalId));
    } else if (!NT_SUCCESS(status) && S->SignallingUp) {
        if (ErrorCode) *ErrorCode = err;
        if (type == AVDTP_MSG_GENERAL_REJECT) PN_WARN("AVDTP %s: general reject", SigName(SignalId));
        else PN_WARN("AVDTP %s rejected, error 0x%02X", SigName(SignalId), err);
    }
    return status;
}

/* Ends the transaction AvdtpTransact left open: its answer is applied, so the receiver's
 * commands that arrived behind it may run (PnAcpMustWait). Releases StreamMutex.
 * NextIntSignal: what the acceptor judges the receiver's commands against until our next
 * command is answered (0: nothing outstanding). */
static VOID AvdtpTxnEndAs(_Inout_ PAVDTP_SESSION S, _In_ UCHAR NextIntSignal)
{
    S->IntSignal = NextIntSignal;
    S->AppliedSeq = S->ResponseSeq;   /* stable: no answer is matched while Txn.Active is FALSE */
    KeSetEvent(&S->TxnApplied, 0, FALSE);
    PnMutexRelease(&S->StreamMutex);
}

static VOID AvdtpTxnEnd(_Inout_ PAVDTP_SESSION S)
{
    AvdtpTxnEndAs(S, 0);
}

/* ------------------------------------------------------------------------- */
/* Capability parsing                                                           */
/* ------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------- */
/* Receive path (DISPATCH): header parsing, reassembly, routing                 */
/* ------------------------------------------------------------------------- */

static IO_WORKITEM_ROUTINE AvdtpAcpWorkItem;
static VOID AvdtpAcpWorkItem(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (!S) return;
    AvdtpAcpWorker(S);
    ExReleaseRundownProtection(&S->WorkRundown);   /* last access */
}

static VOID AvdtpHandleMessage(_Inout_ PAVDTP_SESSION S, _In_ UCHAR Label, _In_ UCHAR MsgType, _In_ UCHAR SignalId,
                               _In_reads_bytes_(Len) const UCHAR *Payload, _In_ ULONG Len)
{
    KIRQL irql;
    if (MsgType == AVDTP_MSG_COMMAND) {
        PAVDTP_ACP_ITEM item = (PAVDTP_ACP_ITEM)PnAllocNp(sizeof(*item), PNA2DP_TAG_AVDTP);
        if (!item) return;
        item->Label = Label;
        item->SignalId = SignalId;
        item->Generation = S->Signalling.Generation;
        item->AfterResponse = S->ResponseSeq;   /* same receive path (ReasmLock) as the answers */
        item->Length = PN_MIN(Len, AVDTP_MAX_SIGNAL_PAYLOAD);
        RtlCopyMemory(item->Payload, Payload, item->Length);
        KeAcquireSpinLock(&S->AcpQueueLock, &irql);
        if (S->AcpQueueDepth >= 32) {
            KeReleaseSpinLock(&S->AcpQueueLock, irql);
            PnFree(item, PNA2DP_TAG_AVDTP);
            return;
        }
        InsertTailList(&S->AcpQueue, &item->Link);
        S->AcpQueueDepth++;
        S->AcpQueuedSeq++;                      /* an answer after it waits for it (AvdtpWaitAcpBefore) */
        KeReleaseSpinLock(&S->AcpQueueLock, irql);
        if (InterlockedCompareExchange(&S->AcpWorkQueued, 1, 0) == 0) {
            if (S->AcpWorkItem && ExAcquireRundownProtection(&S->WorkRundown)) {
                IoQueueWorkItem(S->AcpWorkItem, AvdtpAcpWorkItem, DelayedWorkQueue, S);
            } else {
                InterlockedExchange(&S->AcpWorkQueued, 0);   /* shutting down: AvdtpUninitialize frees the queue */
            }
        }
        return;
    }
    /* response */
    KeAcquireSpinLock(&S->Lock, &irql);
    if (S->Txn.Active && S->Txn.Label == Label && S->Txn.SignalId == SignalId && S->Txn.MessageType == 0xff) {
        S->Txn.MessageType = MsgType;
        S->Txn.PayloadLength = PN_MIN(Len, AVDTP_MAX_SIGNAL_PAYLOAD);
        RtlCopyMemory(S->Txn.Payload, Payload, S->Txn.PayloadLength);
        S->Txn.ErrorCode = (MsgType == AVDTP_MSG_RESPONSE_REJECT && Len) ? Payload[Len - 1] : 0;
        /* SET_CONFIGURATION / RECONFIGURE rejects carry [category][error] */
        if (MsgType == AVDTP_MSG_RESPONSE_REJECT && Len >= 2 && (SignalId == AVDTP_SET_CONFIGURATION || SignalId == AVDTP_RECONFIGURE)) {
            S->Txn.ErrorCode = Payload[1];
        }
        S->ResponseSeq++;                     /* commands behind it wait until it is applied */
        S->Txn.AcpBefore = S->AcpQueuedSeq;   /* commands before it are finished first */
        KeClearEvent(&S->TxnApplied);
        KeSetEvent(&S->Txn.Done, 0, FALSE);
        KeReleaseSpinLock(&S->Lock, irql);
    } else {
        KeReleaseSpinLock(&S->Lock, irql);
        PN_WARN("AVDTP stray response label %u signal %s", Label, SigName(SignalId));
    }
}

static VOID AvdtpSignallingReceive(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    KIRQL irql;
    KeAcquireSpinLock(&S->ReasmLock, &irql);
    AVDTP_REASSEMBLY *r = &S->Reasm;
    if (AvdtpReassemble(r, Data, Len) == AvdtpComplete)
        AvdtpHandleMessage(S, r->Label, r->MessageType, r->SignalId, r->Buffer, r->Length);
    KeReleaseSpinLock(&S->ReasmLock, irql);
}

static VOID AvdtpSignallingClosed(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_ NTSTATUS Status, _In_ ULONG Generation)
{
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (Generation != Channel->Generation) return;   /* an earlier connection: already replaced */
    PN_INFO("signalling channel down 0x%08x", Status);
    S->SignallingUp = FALSE;
    S->DownStatus = Status;
    S->SigDownGen = Generation;
    KeSetEvent(&S->Txn.Done, 0, FALSE);       /* unblock any waiter */
    AvdtpQueueEvent(S, AVDTP_EVT_SIG_DOWN);
}

static VOID AvdtpMediaReceive(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (S->Cb.OnMediaPacket) S->Cb.OnMediaPacket(S->Cb.Context, Data, Len);
}

static VOID AvdtpMediaClosed(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_ NTSTATUS Status, _In_ ULONG Generation)
{
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (Generation != Channel->Generation) return;   /* an earlier connection: already replaced */
    PN_INFO("media channel down 0x%08x", Status);
    S->MediaUp = FALSE;
    S->DownStatus = Status;
    S->MediaDownGen = Generation;
    AvdtpQueueEvent(S, AVDTP_EVT_MEDIA_DOWN);
}

static VOID AvdtpMediaWriteDone(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_ NTSTATUS Status, _In_ ULONG Bytes, _In_ LONGLONG SubmitTimeHns)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    /* The device layer owns the pump; forward via the device's media write callback. */
    extern VOID DeviceMediaWriteDone(PVOID Ctx, NTSTATUS Status, ULONG Bytes, LONGLONG SubmitTimeHns);
    DeviceMediaWriteDone(S->Cb.Context, Status, Bytes, SubmitTimeHns);
}

/* ------------------------------------------------------------------------- */
/* Deferred events (PASSIVE)                                                    */
/* ------------------------------------------------------------------------- */

typedef enum { AcceptReady, AcceptFailed, AcceptStale } AVDTP_ACCEPT_RESULT;
static VOID AvdtpResetStream(_Inout_ PAVDTP_SESSION S);

/* PASSIVE: wait for the accept of a remote-opened channel to complete, then start its
 * readers. Stale: the accepted connection was replaced (by our own connect or a newer
 * accept) before this ran, so this event no longer describes the channel. */
static AVDTP_ACCEPT_RESULT AvdtpOnAcceptedChannelReady(_Inout_ PAVDTP_SESSION S, _In_ BOOLEAN Media, _Out_ PULONG EvalGen)
{
    PPN_L2CAP_CHANNEL ch = Media ? &S->Media : &S->Signalling;
    ULONG gen = Media ? S->MediaAcceptGen : S->SigAcceptGen;
    *EvalGen = gen;
    if (ch->Generation != gen || !ch->RemoteInitiated) return AcceptStale;
    PULONG done = Media ? &S->MediaUpDoneGen : &S->SigUpDoneGen;
    if (*done == gen) return AcceptStale;   /* bit set again for a generation already handled */
    *done = gen;                            /* single worker instance: no lock needed */
    LARGE_INTEGER t;
    t.QuadPart = -PN_HNS_PER_MS * 10000;
    NTSTATUS wait = KeWaitForSingleObject(&ch->ConnectedEvent, Executive, KernelMode, FALSE, &t);
    if (ch->Generation != gen) return AcceptStale;
    if (wait == STATUS_SUCCESS && ch->OpenStatus != STATUS_PENDING && NT_SUCCESS(ch->OpenStatus) && PnChannelIsConnected(ch)) {
        /* a new signalling session: the stream of the previous one is gone. Reset it before
         * the readers run, so no command of the new session sees stale stream state. */
        if (!Media && (S->StreamState != AvdtpStateIdle || S->ResetPending)) AvdtpResetStream(S);
        /* a stale down handled meanwhile may have cleared the flag of this connection */
        KIRQL irql;
        KeAcquireSpinLock(&ch->Lock, &irql);
        if (ch->Generation == gen && ch->State == PnChannelConnected) { if (Media) S->MediaUp = TRUE; else S->SignallingUp = TRUE; }
        KeReleaseSpinLock(&ch->Lock, irql);
        PnChannelStartReaders(ch);   /* idempotent per connection; stops if it was replaced */
        return AcceptReady;
    }
    PN_WARN("accepted %s channel did not open (wait 0x%08x, status 0x%08x)", Media ? "media" : "signalling", wait, ch->OpenStatus);
    /* no new session after all: commands fail as DISCONNECTED until signalling is up again */
    if (!Media) InterlockedExchange(&S->ResetPending, 0);
    /* a late open completes into Disconnecting and closes itself instead of being orphaned */
    if (PnChannelDisconnectGen(ch, gen) || ch->Generation == gen) {
        if (Media) S->MediaUp = FALSE; else S->SignallingUp = FALSE;
    }
    return AcceptFailed;
}

/* Signalling of an earlier connection went down after the remote had already connected
 * again: the AVDTP stream of the old session is gone, the new signalling channel stays.
 * No INT procedure of the old session may run on: ResetPending (set when the new session
 * was accepted, or here) fails every command before it is sent (CANCELLED), the
 * transaction already waiting is woken (CANCELLED) and the media connect is moot
 * (cancelled; AvdtpOpen sends no ABORT for it). The command lock then waits for the
 * procedure to return; the flag is cleared once the stream is reset (review #1). */
static VOID AvdtpResetStream(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    KIRQL irql;
    InterlockedExchange(&S->ResetPending, 1);
    /* under Lock: AvdtpTransact either sees the flag or has cleared the event already */
    KeAcquireSpinLock(&S->Lock, &irql);
    KeSetEvent(&S->Txn.Done, 0, FALSE);
    KeReleaseSpinLock(&S->Lock, irql);
    PnChannelCancelConnect(&S->Media);
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateClosing, TRUE);
    S->MediaExpected = FALSE;
    PnMutexRelease(&S->StreamMutex);
    PnChannelDisconnectSync(&S->Media);
    PnMutexAcquire(&S->StreamMutex);
    S->MediaUp = FALSE;
    S->MediaExpected = FALSE;
    S->RemoteSepsValid = FALSE;
    S->RemoteSepCount = 0;
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateIdle, TRUE);
    InterlockedExchange(&S->ResetPending, 0);   /* the new session's commands may go out */
    PnMutexRelease(&S->StreamMutex);
    PnMutexRelease(&S->Mutex);
}

static IO_WORKITEM_ROUTINE AvdtpEventWorker;
static VOID AvdtpEventWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (!S) return;
    /* Only one instance exists at a time: AVDTP_EVT_RUNNING stays set from queueing until
     * this loop sees no more bits, so the work item is never queued while still queued. */
    for (;;) {
        LONG bits = InterlockedAnd(&S->DownQueued, AVDTP_EVT_RUNNING) & ~AVDTP_EVT_RUNNING;
        if (bits == 0) {
            if (InterlockedCompareExchange(&S->DownQueued, 0, AVDTP_EVT_RUNNING) == AVDTP_EVT_RUNNING) break;
            continue;   /* new bits arrived */
        }
        /* Downs first. Each event names the connection (generation) it is about; one whose
         * channel has been reconnected since is stale and must not touch the new one. */
        if (bits & AVDTP_EVT_MEDIA_DOWN) {
            ULONG gen = S->MediaDownGen;
            if (gen == S->Media.Generation) {
                /* the callback takes the device StateMutex, which ranks above the AVDTP
                 * locks, and re-checks the generation under it; no AVDTP lock is held */
                if (S->Cb.OnMediaDown) S->Cb.OnMediaDown(S->Cb.Context, S->DownStatus, gen);
                PnMutexAcquire(&S->StreamMutex);
                /* Opening is left to MEDIA_UP / AvdtpOpen, which own the new transport */
                if ((S->StreamState == AvdtpStateStreaming || S->StreamState == AvdtpStateOpen) && !PnChannelIsConnected(&S->Media)) {
                    SetStreamState(S, S->SignallingUp ? AvdtpStateConfigured : AvdtpStateIdle, TRUE);
                }
                PnMutexRelease(&S->StreamMutex);
            }
        }
        if (bits & AVDTP_EVT_SIG_DOWN) {
            /* a stale one needs nothing: the new session resets the stream when it starts */
            ULONG gen = S->SigDownGen;
            if (gen == S->Signalling.Generation && S->Cb.OnSignallingDown) S->Cb.OnSignallingDown(S->Cb.Context, S->DownStatus, gen);
        }
        if (bits & AVDTP_EVT_SIG_UP) {
            ULONG gen;
            AvdtpOnAcceptedChannelReady(S, FALSE, &gen);
        }
        if (bits & AVDTP_EVT_MEDIA_UP) {
            ULONG gen;
            AVDTP_ACCEPT_RESULT r = AvdtpOnAcceptedChannelReady(S, TRUE, &gen);
            if (r != AcceptStale) {
                PnMutexAcquire(&S->StreamMutex);
                if (S->Media.Generation == gen) {   /* else a newer transport owns the decision */
                    BOOLEAN ready = r == AcceptReady && PnChannelIsConnected(&S->Media);
                    switch (PnMediaUpAction(ready, S->StreamState, S->OpenSent)) {
                    case PnMediaUpOpenStream:
                        SetStreamState(S, AvdtpStateOpen, TRUE);
                        break;
                    case PnMediaUpHold:
                        /* the receiver opened it right behind accepting our OPEN, whose
                         * answer AvdtpOpen has not applied yet: it adopts the channel (T9) */
                        S->MediaHeldGen = gen;
                        break;
                    default:
                        if (r == AcceptReady && PnChannelDisconnectGen(&S->Media, gen)) {
                            /* the stream was aborted or closed while the transport was opening */
                            S->MediaUp = FALSE;
                        } else if (r == AcceptReady) {
                            /* already closing (AvdtpOpen dropped it): its flag goes with it,
                             * unless a newer connection took the channel (review round 3 #2) */
                            KIRQL irql;
                            KeAcquireSpinLock(&S->Media.Lock, &irql);
                            if (S->Media.Generation == gen && S->Media.State != PnChannelConnected) S->MediaUp = FALSE;
                            KeReleaseSpinLock(&S->Media.Lock, irql);
                        }
                        if (S->StreamState == AvdtpStateOpening) SetStreamState(S, S->SignallingUp ? AvdtpStateConfigured : AvdtpStateIdle, TRUE);
                        break;
                    }
                }
                PnMutexRelease(&S->StreamMutex);
            }
        }
    }
    ExReleaseRundownProtection(&S->WorkRundown);   /* last access to S */
}

static VOID AvdtpQueueEvent(_Inout_ PAVDTP_SESSION S, _In_ ULONG Bits)
{
    LONG old, next;
    do {
        old = S->DownQueued;
        next = old | (LONG)Bits | AVDTP_EVT_RUNNING;
    } while (InterlockedCompareExchange(&S->DownQueued, next, old) != old);
    if (!(old & AVDTP_EVT_RUNNING)) {
        if (S->DownWorkItem && ExAcquireRundownProtection(&S->WorkRundown)) {
            IoQueueWorkItem(S->DownWorkItem, AvdtpEventWorker, DelayedWorkQueue, S);
        } else {
            InterlockedExchange(&S->DownQueued, 0);   /* shutting down: events are moot */
        }
    }
}

/* ------------------------------------------------------------------------- */
/* ACP command processing (PASSIVE)                                             */
/* ------------------------------------------------------------------------- */

static PAVDTP_LOCAL_SEP FindLocalSep(_In_ PAVDTP_SESSION S, _In_ UCHAR Seid)
{
    for (ULONG i = 0; i < S->LocalSepCount; i++) if (S->LocalSeps[i].Seid == Seid) return &S->LocalSeps[i];
    return NULL;
}

/* Validation and the response come from the pure AvdtpAcpProcess (avdtp_wire.cpp);
 * this applies its effects to the session in the documented order. StreamMutex held,
 * never the command lock: answers go out while a command of ours is outstanding (T9).
 * Returns TRUE when the media channel must be closed before *NewState applies; the
 * caller does that outside StreamMutex (AvdtpAcpCloseMedia). */
static BOOLEAN AvdtpProcessCommand(_Inout_ PAVDTP_SESSION S, _In_ PAVDTP_ACP_ITEM C, _Out_ AVDTP_STREAM_STATE *MidState,
                                   _Out_ AVDTP_STREAM_STATE *NewState)
{
    AVDTP_ACP_CONTEXT ctx;
    AVDTP_ACP_RESULT r;
    PN_VERB("AVDTP <- %s label %u len %u", SigName(C->SignalId), C->Label, C->Length);
    ctx.LocalSeps = S->LocalSeps;
    ctx.LocalSepCount = S->LocalSepCount;
    ctx.StreamState = S->StreamState;
    ctx.LocalSeid = S->LocalSeid;
    ctx.RemoteSeid = S->RemoteSeid;
    ctx.ConfiguredIes = S->ConfiguredIes;
    ctx.ConfiguredIesLength = S->ConfiguredIesLength;
    ctx.DelayReportingNegotiated = S->DelayReportingNegotiated;
    ctx.IntSignal = S->IntSignal;
    ctx.CheckConfiguration = S->Cb.OnRemoteSetConfiguration;
    ctx.CheckReconfigure = S->Cb.OnRemoteReconfigure;
    ctx.CheckContext = S->Cb.Context;
    AvdtpAcpProcess(&ctx, C->SignalId, C->Payload, C->Length, &r);

    if (r.Effects & AVDTP_ACP_FX_STORE_CONFIG) {
        S->LocalSeid = r.ConfigLocalSeid;
        S->RemoteSeid = r.ConfigRemoteSeid;
        S->ConfiguredIesLength = r.IesLength;
        RtlCopyMemory(S->ConfiguredIes, r.Ies, r.IesLength);
        S->ConfiguredServiceCaps = r.ServiceCaps;
        S->DelayReportingNegotiated = r.DelayReporting;
    }
    if (r.Effects & AVDTP_ACP_FX_STORE_IES) {
        S->ConfiguredIesLength = r.IesLength;
        RtlCopyMemory(S->ConfiguredIes, r.Ies, r.IesLength);
    }
    if (r.Effects & AVDTP_ACP_FX_MEDIA_EXPECTED) S->MediaExpected = TRUE;
    if (r.Effects & AVDTP_ACP_FX_OPEN_COLLISION) S->OpenCollided = TRUE;
    if (r.Effects & AVDTP_ACP_FX_RESET_DELAY) S->SinkDelayTenthsMs = 0;
    if (r.Effects & AVDTP_ACP_FX_STORE_DELAY) S->SinkDelayTenthsMs = r.DelayTenthsMs;
    if (r.MessageType == AVDTP_MSG_GENERAL_REJECT) AvdtpWriteMessage(S, AVDTP_MSG_GENERAL_REJECT, C->Label, C->SignalId, NULL, 0);
    else AvdtpSendResponse(S, C->Label, C->SignalId, r.MessageType == AVDTP_MSG_RESPONSE_ACCEPT, r.Response, r.ResponseLength);
    if ((r.Effects & (AVDTP_ACP_FX_REMOTE_START | AVDTP_ACP_FX_REMOTE_SUSPEND)) && S->Cb.OnRemoteStreamControl)
        S->Cb.OnRemoteStreamControl(S->Cb.Context, (r.Effects & AVDTP_ACP_FX_REMOTE_START) != 0);
    if (r.Effects & AVDTP_ACP_FX_RELEASE_REMOTE) AvdtpReleaseRemoteSep(S, S->RemoteSeid);
    if (r.Effects & AVDTP_ACP_FX_MID_STATE) SetStreamState(S, r.MidState, TRUE);
    *MidState = r.MidState;
    *NewState = r.NewState;
    /* CLOSE / ABORT: the media channel closes outside StreamMutex, then NewState (no
     * delay callback goes with these) */
    if (r.Effects & AVDTP_ACP_FX_DISCONNECT_MEDIA) return TRUE;
    if (r.Effects & AVDTP_ACP_FX_NEW_STATE) SetStreamState(S, r.NewState, TRUE);
    if ((r.Effects & AVDTP_ACP_FX_DELAY_CALLBACK) && S->Cb.OnDelayReport) S->Cb.OnDelayReport(S->Cb.Context, r.DelayTenthsMs);
    return FALSE;
}

/* The receiver closed or aborted the stream: drop the media channel (a wait, so without
 * StreamMutex), then finish the transition unless the stream has moved on meanwhile. */
static VOID AvdtpAcpCloseMedia(_Inout_ PAVDTP_SESSION S, _In_ AVDTP_STREAM_STATE MidState, _In_ AVDTP_STREAM_STATE NewState)
{
    PAGED_CODE();
    ULONG gen = S->Media.Generation;
    PnChannelDisconnectSync(&S->Media);
    PnMutexAcquire(&S->StreamMutex);
    if (S->Media.Generation == gen) {   /* else a newer transport was accepted meanwhile */
        S->MediaUp = FALSE;
        S->MediaExpected = FALSE;
    }
    if (S->StreamState == MidState) SetStreamState(S, NewState, TRUE);
    PnMutexRelease(&S->StreamMutex);
}

static VOID AvdtpAcpWorker(_In_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    for (;;) {
        KIRQL irql;
        PAVDTP_ACP_ITEM item = NULL;
        KeAcquireSpinLock(&S->AcpQueueLock, &irql);
        if (!IsListEmpty(&S->AcpQueue)) {
            PLIST_ENTRY e = RemoveHeadList(&S->AcpQueue);
            item = CONTAINING_RECORD(e, AVDTP_ACP_ITEM, Link);
            S->AcpQueueDepth--;
        } else {
            InterlockedExchange(&S->AcpWorkQueued, 0);
        }
        KeReleaseSpinLock(&S->AcpQueueLock, irql);
        if (!item) break;
        /* A command that arrived behind the answer to ours must see that answer applied
         * (e.g. the receiver's OPEN right behind accepting our SET_CONFIGURATION). Only
         * scheduling separates the two (our INT thread takes StreamMutex as it wakes), so
         * the wait is short; bounded at 1 s all the same. */
        for (ULONG i = 0; i < 40 && PnAcpMustWait(item->AfterResponse, S->AppliedSeq); i++) {
            LARGE_INTEGER t;
            t.QuadPart = -PN_HNS_PER_MS * 25;
            KeWaitForSingleObject(&S->TxnApplied, Executive, KernelMode, FALSE, &t);
        }
        /* Our OPEN's media connect or the disconnect after our CLOSE / ABORT is running: a
         * command whose answer depends on the stream state is judged once that settled
         * (Open / Idle), not against Opening / Closing; bounded at 1 s (review round 4 #2). */
        if (PnAcpWaitsForTransport(item->SignalId, KeReadStateEvent(&S->TransportIdle) == 0)) {
            LARGE_INTEGER t;
            t.QuadPart = -PN_HNS_PER_MS * 1000;
            KeWaitForSingleObject(&S->TransportIdle, Executive, KernelMode, FALSE, &t);
        }
        AVDTP_STREAM_STATE mid = AvdtpStateIdle, next = AvdtpStateIdle;
        BOOLEAN closeMedia = FALSE;
        PnMutexAcquire(&S->StreamMutex);
        if (S->SignallingUp && item->Generation == S->Signalling.Generation) closeMedia = AvdtpProcessCommand(S, item, &mid, &next);
        PnMutexRelease(&S->StreamMutex);
        if (closeMedia) AvdtpAcpCloseMedia(S, mid, next);
        PnFree(item, PNA2DP_TAG_AVDTP);
        InterlockedIncrement(&S->AcpDoneSeq);
        KeSetEvent(&S->AcpProgress, 0, FALSE);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                    */
/* ------------------------------------------------------------------------- */

/* On failure everything is undone and S->Link stays NULL, so the device teardown skips the
 * session entirely. */
NTSTATUS AvdtpInitialize(_Out_ PAVDTP_SESSION S, _In_ PPN_BTH_LINK Link, _In_ PDEVICE_OBJECT DeviceObject, _In_ const AVDTP_CALLBACKS *Cb)
{
    PAGED_CODE();
    NTSTATUS status;
    RtlZeroMemory(S, sizeof(*S));
    S->Cb = *Cb;
    S->DeviceObject = DeviceObject;
    ExInitializeFastMutex(&S->Mutex);
    ExInitializeFastMutex(&S->StreamMutex);
    KeInitializeEvent(&S->TxnApplied, NotificationEvent, TRUE);
    KeInitializeEvent(&S->TransportIdle, NotificationEvent, TRUE);
    KeInitializeEvent(&S->AcpProgress, NotificationEvent, FALSE);
    KeInitializeSpinLock(&S->Lock);
    KeInitializeSpinLock(&S->ReasmLock);
    KeInitializeSpinLock(&S->AcpQueueLock);
    InitializeListHead(&S->AcpQueue);
    KeInitializeEvent(&S->Txn.Done, NotificationEvent, FALSE);
    ExInitializeRundownProtection(&S->WorkRundown);
    S->WorkReady = TRUE;
    S->RemoteVersion = 0x0103;
    S->AcpWorkItem = IoAllocateWorkItem(DeviceObject);
    S->DownWorkItem = IoAllocateWorkItem(DeviceObject);
    if (!S->AcpWorkItem || !S->DownWorkItem) { status = STATUS_INSUFFICIENT_RESOURCES; goto fail; }

    status = PnChannelInitialize(&S->Signalling, Link, FALSE, PN_L2CAP_SIG_MTU, PN_L2CAP_SIG_MTU,
                                 AvdtpSignallingReceive, AvdtpSignallingClosed, NULL, S);
    if (!NT_SUCCESS(status)) goto fail;
    status = PnChannelInitialize(&S->Media, Link, TRUE, 2048, 2048, AvdtpMediaReceive, AvdtpMediaClosed, AvdtpMediaWriteDone, S);
    if (!NT_SUCCESS(status)) goto fail;
    S->Link = Link;
    return STATUS_SUCCESS;

fail:
    /* no channel was ever connected, so no callback or work item can be pending */
    PnChannelUninitialize(&S->Media);        /* no-op unless PnChannelInitialize ran */
    PnChannelUninitialize(&S->Signalling);
    if (S->AcpWorkItem) { IoFreeWorkItem(S->AcpWorkItem); S->AcpWorkItem = NULL; }
    if (S->DownWorkItem) { IoFreeWorkItem(S->DownWorkItem); S->DownWorkItem = NULL; }
    S->Link = NULL;
    return status;
}

VOID AvdtpStopWork(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (!S->WorkReady || S->WorkStopped) return;
    S->WorkStopped = TRUE;
    ExWaitForRundownProtectionRelease(&S->WorkRundown);
}

VOID AvdtpUninitialize(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (!S->Link) return;
    AvdtpDisconnect(S);
    PnChannelUninitialize(&S->Media);
    PnChannelUninitialize(&S->Signalling);
    AvdtpStopWork(S);   /* channels are closed: nothing can queue work any more */
    /* drain ACP queue */
    for (;;) {
        KIRQL irql;
        PAVDTP_ACP_ITEM item = NULL;
        KeAcquireSpinLock(&S->AcpQueueLock, &irql);
        if (!IsListEmpty(&S->AcpQueue)) item = CONTAINING_RECORD(RemoveHeadList(&S->AcpQueue), AVDTP_ACP_ITEM, Link);
        KeReleaseSpinLock(&S->AcpQueueLock, irql);
        if (!item) break;
        PnFree(item, PNA2DP_TAG_AVDTP);
    }
    if (S->AcpWorkItem) { IoFreeWorkItem(S->AcpWorkItem); S->AcpWorkItem = NULL; }
    if (S->DownWorkItem) { IoFreeWorkItem(S->DownWorkItem); S->DownWorkItem = NULL; }
    S->Link = NULL;
}

VOID AvdtpSetLocalSeps(_Inout_ PAVDTP_SESSION S, _In_reads_(Count) const AVDTP_LOCAL_SEP *Seps, _In_ ULONG Count)
{
    Count = PN_MIN(Count, AVDTP_LOCAL_SEP_COUNT);
    RtlCopyMemory(S->LocalSeps, Seps, Count * sizeof(AVDTP_LOCAL_SEP));
    S->LocalSepCount = Count;
}

/* A single byte store: the acceptor reads it under StreamMutex and cannot see a torn value. */
VOID AvdtpSetLocalSepAvailable(_Inout_ PAVDTP_SESSION S, _In_ ULONG Codec, _In_ BOOLEAN Available)
{
    for (ULONG i = 0; i < S->LocalSepCount && i < AVDTP_LOCAL_SEP_COUNT; i++)
        if (S->LocalSeps[i].Codec == Codec) S->LocalSeps[i].InUse = Available ? 0 : 1;
}

NTSTATUS AvdtpConnectSignalling(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (S->SignallingUp) return STATUS_SUCCESS;
    NTSTATUS status = PnChannelConnect(&S->Signalling, PSM_AVDTP, PN_L2CAP_SIG_MTU, 10000);
    if (!NT_SUCCESS(status)) {
        PN_WARN("AVDTP signalling connect failed 0x%08x", status);
        return status;
    }
    /* a new signalling session: the stream of the previous one is gone */
    if (S->StreamState != AvdtpStateIdle || S->ResetPending) AvdtpResetStream(S);
    S->SignallingUp = TRUE;
    S->RemoteSignallingInitiated = FALSE;
    S->Reasm.Active = FALSE;
    PnChannelStartReaders(&S->Signalling);
    return STATUS_SUCCESS;
}

NTSTATUS AvdtpAcceptIncoming(_Inout_ PAVDTP_SESSION S, _In_ PINDICATION_PARAMETERS Params, _Out_ PBOOLEAN Signalling)
{
    NTSTATUS status;
    PN_INCOMING_KIND kind = PnIncomingKind(S->SignallingUp, S->Signalling.State == PnChannelConnecting, S->MediaUp,
                                           S->Media.State == PnChannelConnecting, S->MediaExpected || S->OpenSent, S->StreamState);
    *Signalling = kind == PnIncomingSignalling;
    if (kind == PnIncomingSignalling) {
        S->RemoteSignallingInitiated = TRUE;
        S->Reasm.Active = FALSE;
        status = PnChannelAccept(&S->Signalling, Params, PN_L2CAP_SIG_MTU);
        if (NT_SUCCESS(status)) {
            /* mark up optimistically; readers start when the accept completes (event worker).
             * A stream of the old session is reset first; until then no command of ours
             * goes out on the new channel (review #1). */
            S->SigAcceptGen = S->Signalling.Generation;
            if (S->StreamState != AvdtpStateIdle) InterlockedExchange(&S->ResetPending, 1);
            S->SignallingUp = TRUE;
            AvdtpQueueEvent(S, AVDTP_EVT_SIG_UP);
        } else {
            PnLinkRejectIncoming(S->Link, Params);   /* the indication must get a response */
        }
        return status;
    }
    if (kind == PnIncomingMedia) {
        status = PnChannelAccept(&S->Media, Params, AvdtpPreferredMediaMtu(S));
        if (NT_SUCCESS(status)) {
            S->MediaAcceptGen = S->Media.Generation;
            S->MediaExpected = FALSE;
            S->MediaUp = TRUE;
            AvdtpQueueEvent(S, AVDTP_EVT_MEDIA_UP);
        } else {
            PnLinkRejectIncoming(S->Link, Params);
        }
        return status;
    }
    PN_WARN("unexpected incoming L2CAP connection (sig %u media %u state %u)", S->SignallingUp, S->MediaUp, S->StreamState);
    PnLinkRejectIncoming(S->Link, Params);
    return STATUS_REQUEST_NOT_ACCEPTED;   /* refused: not the receiver connecting (review round 4 #1) */
}

VOID AvdtpDisconnect(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (!S->Link) return;
    PnMutexAcquire(&S->Mutex);   /* no INT procedure in flight; the acceptor keeps running */
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateClosing, FALSE);
    /* clear the up flags only for the connections closed here: a remote reconnect accepted
     * meanwhile (at DISPATCH) owns its own flag */
    ULONG mg = S->Media.Generation, sg = S->Signalling.Generation;
    PnMutexRelease(&S->StreamMutex);
    PnChannelDisconnectSync(&S->Media);
    PnChannelDisconnectSync(&S->Signalling);
    PnMutexAcquire(&S->StreamMutex);
    if (S->Media.Generation == mg) S->MediaUp = FALSE;
    S->MediaExpected = FALSE;
    if (S->Signalling.Generation == sg) S->SignallingUp = FALSE;
    S->RemoteSepsValid = FALSE;
    S->RemoteSepCount = 0;
    S->Reasm.Active = FALSE;
    KeSetEvent(&S->Txn.Done, 0, FALSE);
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateIdle, FALSE);
    InterlockedExchange(&S->ResetPending, 0);   /* nothing of the old session is left to reset */
    PnMutexRelease(&S->StreamMutex);
    PnMutexRelease(&S->Mutex);
}

/* ------------------------------------------------------------------------- */
/* INT procedures                                                               */
/* ------------------------------------------------------------------------- */
/* Shape of each (T9): under the command lock, a short StreamMutex section checks the
 * state and publishes IntSignal; the transaction runs without StreamMutex; AvdtpTransact
 * returns holding it, the answer is judged against the state as it is now (PnIntOutcome)
 * and applied; AvdtpTxnEnd releases it. Channel connects and disconnects run outside
 * StreamMutex. */

/* After CLOSE / ABORT (or locally): drop the media channel and go Idle, unless the
 * receiver set up a new stream meanwhile. Command lock held, StreamMutex not. The caller
 * cleared TransportIdle before its transaction ended; the receiver's state-dependent
 * commands wait for Idle (review round 4 #2). */
static VOID AvdtpFinishTeardown(_Inout_ PAVDTP_SESSION S, _In_ AVDTP_STREAM_STATE Sent)
{
    KeClearEvent(&S->TransportIdle);   /* the local-only close had no transaction */
    PnChannelDisconnectSync(&S->Media);
    PnMutexAcquire(&S->StreamMutex);
    if (PnIntOutcome(AVDTP_CLOSE, STATUS_SUCCESS, Sent, S->StreamState) != PnIntLost) {
        S->MediaUp = FALSE;
        S->MediaExpected = FALSE;
        SetStreamState(S, AvdtpStateIdle, FALSE);
    }
    PnMutexRelease(&S->StreamMutex);
    KeSetEvent(&S->TransportIdle, 0, FALSE);
}

NTSTATUS AvdtpDiscover(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    NTSTATUS status;
    UCHAR err;
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    S->RemoteSepCount = 0;
    S->RemoteSepsValid = FALSE;
    PnMutexRelease(&S->StreamMutex);
    status = AvdtpTransact(S, AVDTP_DISCOVER, NULL, 0, &err);
    if (!NT_SUCCESS(status)) { AvdtpTxnEnd(S); PnMutexRelease(&S->Mutex); return status; }
    ULONG count = AvdtpParseDiscover(S->Txn.Payload, S->Txn.PayloadLength, S->RemoteSeps, AVDTP_MAX_SEPS);
    S->RemoteSepCount = count;
    AvdtpTxnEnd(S);
    PN_INFO("AVDTP discovered %u audio SEPs", count);
    /* capability fallback policy: transport_policy.h */
    PN_CAPS_QUERY q;
    PnCapsQueryStart(&q, S->RemoteVersion, S->CapsBasicOnly);
    ULONG withCaps = 0;
    for (ULONG i = 0; i < count; i++) {
        PAVDTP_REMOTE_SEP sep = &S->RemoteSeps[i];
        UCHAR p[1] = { (UCHAR)(sep->Seid << 2) };
        PN_CAPS_ACTION action = PnCapsNextSep;
        for (ULONG attempt = 0; attempt < 2; attempt++) {
            status = AvdtpTransact(S, PnCapsQuerySignal(&q), p, 1, &err);
            action = PnCapsQueryResult(&q, status);
            /* parsed under StreamMutex: the acceptor clears in-use bits of this array */
            if (action == PnCapsParse && AvdtpParseServiceCapabilities(S->Txn.Payload, S->Txn.PayloadLength, sep)) withCaps++;
            AvdtpTxnEnd(S);
            if (action != PnCapsRetryBasic) break;
        }
        if (action != PnCapsParse) {
            PN_WARN("GET_CAPABILITIES seid %u failed 0x%08x", sep->Seid, status);
            if (action == PnCapsStop) break;
            continue;
        }
        PN_INFO("  SEP %u: %s in-use %u codec %s (type 0x%02X vid %08X cid %04X) svc 0x%02X", sep->Seid,
                sep->Tsep ? "SNK" : "SRC", sep->InUse, A2dpCodecName(sep->Codec), sep->MediaCodecType, sep->VendorId,
                sep->VendorCodecId, sep->ServiceCaps);
    }
    if (q.Downgraded) {
        /* on evidence only (PnCapsQueryResult): remembered for this device */
        S->RemoteVersion = q.Version;
        S->CapsBasicOnly = TRUE;
    }
    PnMutexAcquire(&S->StreamMutex);
    /* a new signalling session reset the list meanwhile: this discovery is void */
    S->RemoteSepsValid = S->RemoteSepCount == count && PnDiscoveryValid(status, count, withCaps);
    BOOLEAN valid = S->RemoteSepsValid;
    PnMutexRelease(&S->StreamMutex);
    PnMutexRelease(&S->Mutex);
    if (valid) return STATUS_SUCCESS;
    PN_WARN("AVDTP discovery: no SEP with usable capabilities (%u SEPs)", count);
    return NT_SUCCESS(status) ? STATUS_NOT_SUPPORTED : status;
}

NTSTATUS AvdtpSetConfiguration(_Inout_ PAVDTP_SESSION S, _In_ UCHAR LocalSeid, _In_ UCHAR RemoteSeid,
                               _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                               _In_ BOOLEAN RequestDelayReporting, _In_ BOOLEAN RequestContentProtection,
                               _Out_opt_ PUCHAR ErrorCode)
{
    PAGED_CODE();
    UCHAR p[64];
    ULONG n = 0, err = 0;
    if (ErrorCode) *ErrorCode = 0;
    UCHAR mct;
    UCHAR dummy[32];
    PAVDTP_LOCAL_SEP local = FindLocalSep(S, LocalSeid);
    if (!local) return STATUS_INVALID_PARAMETER;
    A2dpBuildLocalCaps(local->Codec, dummy, sizeof(dummy), &mct);

    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateIdle) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    S->IntSignal = AVDTP_SET_CONFIGURATION;   /* a receiver SET_CONFIGURATION now is refused SEP_IN_USE */
    PnMutexRelease(&S->StreamMutex);
    p[n++] = (UCHAR)(RemoteSeid << 2);
    p[n++] = (UCHAR)(LocalSeid << 2);
    p[n++] = AVDTP_CAT_MEDIA_TRANSPORT; p[n++] = 0;
    n += AvdtpBuildMediaCodecCap(p + n, sizeof(p) - n, mct, CodecIes, Len);
    if (RequestContentProtection && n + 4 <= sizeof(p)) { p[n++] = AVDTP_CAT_CONTENT_PROTECTION; p[n++] = 2; p[n++] = 0x02; p[n++] = 0x00; } /* SCMS-T */
    if (RequestDelayReporting && n + 2 <= sizeof(p)) { p[n++] = AVDTP_CAT_DELAY_REPORTING; p[n++] = 0; }
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_SET_CONFIGURATION, p, n, &ec);
    err = ec;
    if (ErrorCode) *ErrorCode = ec;
    BOOLEAN abortSep = FALSE;
    switch (PnIntOutcome(AVDTP_SET_CONFIGURATION, status, AvdtpStateIdle, S->StreamState)) {
    case PnIntApply:
        S->LocalSeid = LocalSeid;
        S->RemoteSeid = RemoteSeid;
        S->ConfiguredIesLength = (UCHAR)PN_MIN(Len, sizeof(S->ConfiguredIes));
        RtlCopyMemory(S->ConfiguredIes, CodecIes, S->ConfiguredIesLength);
        S->DelayReportingNegotiated = RequestDelayReporting;
        S->SinkDelayTenthsMs = 0;   /* a report of an earlier stream does not apply (T6) */
        SetStreamState(S, AvdtpStateConfigured, FALSE);
        break;
    case PnIntLost:
        /* the stream moved on under our request: a configuration of the receiver's SEP
         * that we cannot track is aborted below */
        PN_WARN("SET_CONFIGURATION answered after the stream changed (state %u)", S->StreamState);
        abortSep = NT_SUCCESS(status);
        status = STATUS_INVALID_DEVICE_STATE;
        break;
    default:
        PN_WARN("SET_CONFIGURATION failed 0x%08x (avdtp error 0x%02X)", status, err);
        /* the receiver may have configured its SEP without answering: free it (T3) */
        abortSep = status == STATUS_IO_TIMEOUT;
        break;
    }
    /* the ABORT below names the receiver's SEP: its own SET_CONFIGURATION stays refused
     * (SEP_IN_USE) until that ABORT is answered, as during ours (review #2) */
    AvdtpTxnEndAs(S, abortSep ? AVDTP_SET_CONFIGURATION : 0);
    if (abortSep) {
        UCHAR a[1] = { (UCHAR)(RemoteSeid << 2) };
        AvdtpTransact(S, AVDTP_ABORT, a, 1, NULL);
        AvdtpTxnEnd(S);
    }
    PnMutexRelease(&S->Mutex);
    return status;
}

NTSTATUS AvdtpReconfigure(_Inout_ PAVDTP_SESSION S, _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len)
{
    PAGED_CODE();
    UCHAR p[64];
    ULONG n = 0;
    UCHAR mct, dummy[32];
    PAVDTP_LOCAL_SEP local = FindLocalSep(S, S->LocalSeid);
    if (!local) return STATUS_INVALID_DEVICE_STATE;
    A2dpBuildLocalCaps(local->Codec, dummy, sizeof(dummy), &mct);
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateOpen) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    p[n++] = (UCHAR)(S->RemoteSeid << 2);
    S->IntSignal = AVDTP_RECONFIGURE;
    PnMutexRelease(&S->StreamMutex);
    n += AvdtpBuildMediaCodecCap(p + n, sizeof(p) - n, mct, CodecIes, Len);
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_RECONFIGURE, p, n, &ec);
    switch (PnIntOutcome(AVDTP_RECONFIGURE, status, AvdtpStateOpen, S->StreamState)) {
    case PnIntApply:   /* also when the receiver's START crossed an accepted RECONFIGURE (round 5 #2) */
        S->ConfiguredIesLength = (UCHAR)PN_MIN(Len, sizeof(S->ConfiguredIes));
        RtlCopyMemory(S->ConfiguredIes, CodecIes, S->ConfiguredIesLength);
        break;
    case PnIntLost:
        status = STATUS_INVALID_DEVICE_STATE;   /* the receiver closed it, or started it and refused ours, meanwhile */
        break;
    default:
        break;
    }
    AvdtpTxnEnd(S);
    PnMutexRelease(&S->Mutex);
    return status;
}

/* Media channel facts checked under its Lock, so a remote disconnect (DISPATCH, without
 * StreamMutex) is either seen here or clears MediaUp after us (review round 3 #2).
 * Own: our connect is the current, connected one. Otherwise: the receiver's accepted
 * channel is the current one and still connecting or connected. */
static BOOLEAN AvdtpMediaChannelIs(_Inout_ PAVDTP_SESSION S, _In_ BOOLEAN Own)
{
    PPN_L2CAP_CHANNEL ch = &S->Media;
    KIRQL irql;
    KeAcquireSpinLock(&ch->Lock, &irql);
    BOOLEAN r = Own ? PnOwnMediaLive(ch->State == PnChannelConnected, ch->RemoteInitiated)
                    : PnRemoteMediaCurrent(ch->RemoteInitiated, ch->Generation == S->MediaAcceptGen,
                                           ch->State == PnChannelConnecting || ch->State == PnChannelConnected);
    KeReleaseSpinLock(&ch->Lock, irql);
    return r;
}

/* AvdtpOpen after an applied OPEN answer: connect the media channel and open the stream.
 * Command lock held, StreamMutex not; TransportIdle is clear (review round 4 #2). */
static NTSTATUS AvdtpOpenConnectMedia(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    NTSTATUS status = PnChannelConnect(&S->Media, PSM_AVDTP, AvdtpPreferredMediaMtu(S), 10000);
    PnMutexAcquire(&S->StreamMutex);
    BOOLEAN opening = S->StreamState == AvdtpStateOpening;
    if (NT_SUCCESS(status) && opening) {
        /* the receiver may have closed it already (its down event cleared MediaUp): a
         * connect that failed, not a transport (review round 3 #2) */
        if (AvdtpMediaChannelIs(S, TRUE)) S->MediaUp = TRUE;
        else status = STATUS_CONNECTION_DISCONNECTED;
    }
    PnMutexRelease(&S->StreamMutex);
    if (NT_SUCCESS(status) && !opening) {
        /* the receiver aborted or closed the stream while we connected */
        PnChannelDisconnectSync(&S->Media);
        return STATUS_INVALID_DEVICE_STATE;
    }
    if (!NT_SUCCESS(status)) {
        if (!opening) return STATUS_INVALID_DEVICE_STATE;
        switch (PnMediaConnectFailureAction(status, S->MediaUp && AvdtpMediaChannelIs(S, FALSE), S->ResetPending != 0)) {
        case PnMediaConnectAdopt:
            /* the receiver's own media channel completes the open */
            return STATUS_PENDING;
        case PnMediaConnectReset:
            /* cancelled for a new signalling session: no ABORT on its channel (review #1) */
            return STATUS_CANCELLED;
        default:
            break;
        }
        PN_WARN("media channel connect failed 0x%08x; aborting stream", status);
        KeSetEvent(&S->TransportIdle, 0, FALSE);   /* nothing left to wait for; our ABORT orders itself */
        PnMutexAcquire(&S->StreamMutex);
        BOOLEAN abort = S->StreamState == AvdtpStateOpening;
        /* left over from a channel that is gone: it would refuse the receiver's next transport */
        if (S->MediaUp && !AvdtpMediaChannelIs(S, FALSE)) S->MediaUp = FALSE;
        UCHAR a[1] = { (UCHAR)(S->RemoteSeid << 2) };
        if (abort) { S->IntSignal = AVDTP_ABORT; SetStreamState(S, AvdtpStateAborting, FALSE); }
        PnMutexRelease(&S->StreamMutex);
        if (abort) {
            NTSTATUS as = AvdtpTransact(S, AVDTP_ABORT, a, 1, NULL);
            if (PnIntOutcome(AVDTP_ABORT, as, AvdtpStateAborting, S->StreamState) != PnIntLost) SetStreamState(S, AvdtpStateIdle, FALSE);
            AvdtpTxnEnd(S);
        }
        return status;
    }
    PnChannelStartReaders(&S->Media);
    PnMutexAcquire(&S->StreamMutex);
    opening = S->StreamState == AvdtpStateOpening;
    if (opening) SetStreamState(S, AvdtpStateOpen, FALSE);
    PnMutexRelease(&S->StreamMutex);
    /* not Opening any more: the receiver's ABORT / CLOSE closed the channel meanwhile */
    return opening ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
}

NTSTATUS AvdtpOpen(_Inout_ PAVDTP_SESSION S, _Out_opt_ PUCHAR ErrorCode, _Out_opt_ PBOOLEAN ReceiverOpenRefused)
{
    PAGED_CODE();
    if (ErrorCode) *ErrorCode = 0;
    if (ReceiverOpenRefused) *ReceiverOpenRefused = FALSE;
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateConfigured) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    S->IntSignal = AVDTP_OPEN;   /* a receiver OPEN now is refused: one side connects the transport */
    S->OpenSent = TRUE;          /* a receiver may open the transport before our OPEN's answer arrives */
    S->OpenCollided = FALSE;
    PnMutexRelease(&S->StreamMutex);
    NTSTATUS status = AvdtpTransact(S, AVDTP_OPEN, p, 1, ErrorCode);
    BOOLEAN connect = FALSE;
    S->OpenSent = FALSE;
    if (ReceiverOpenRefused) *ReceiverOpenRefused = S->OpenCollided;   /* StreamMutex held */
    switch (PnIntOutcome(AVDTP_OPEN, status, AvdtpStateConfigured, S->StreamState)) {
    case PnIntApply:
        SetStreamState(S, AvdtpStateOpening, FALSE);
        /* INT opens the transport channel, unless the receiver already did (T9) */
        /* a MediaUp left behind by a channel that is gone is no transport (review round 3 #2) */
        switch (PnOpenTransport(S->MediaUp, AvdtpMediaChannelIs(S, FALSE),
                                S->MediaHeldGen == S->Media.Generation && PnChannelIsConnected(&S->Media))) {
        case PnOpenAdoptReady:
            SetStreamState(S, AvdtpStateOpen, FALSE);   /* its channel is up, readers running */
            status = STATUS_SUCCESS;
            break;
        case PnOpenAdoptPending:
            status = STATUS_PENDING;                    /* its accept completes and opens the stream */
            break;
        default:
            connect = TRUE;
            break;
        }
        break;
    case PnIntReached:
        status = STATUS_SUCCESS;   /* the receiver opened it meanwhile */
        break;
    case PnIntPending:
        status = STATUS_PENDING;   /* the receiver's OPEN: its transport completes it */
        break;
    case PnIntLost:
        status = STATUS_INVALID_DEVICE_STATE;
        break;
    default:
        /* refused or unanswered: a transport the receiver opened for it is not ours */
        if (S->MediaUp && S->StreamState == AvdtpStateConfigured && PnChannelDisconnectGen(&S->Media, S->Media.Generation)) S->MediaUp = FALSE;
        break;
    }
    /* review round 4 #2: the receiver's state-dependent commands wait for the connect */
    if (connect) KeClearEvent(&S->TransportIdle);
    AvdtpTxnEnd(S);
    if (!connect) { PnMutexRelease(&S->Mutex); return status; }
    /* a new signalling session took over: AvdtpResetStream ends this stream (review #1) */
    status = S->ResetPending ? STATUS_CANCELLED : AvdtpOpenConnectMedia(S);
    KeSetEvent(&S->TransportIdle, 0, FALSE);
    PnMutexRelease(&S->Mutex);
    return status;
}

NTSTATUS AvdtpStart(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    AVDTP_STREAM_STATE st = S->StreamState;
    if (st != AvdtpStateOpen) {
        PnMutexRelease(&S->StreamMutex);
        PnMutexRelease(&S->Mutex);
        return st == AvdtpStateStreaming ? STATUS_SUCCESS : STATUS_INVALID_DEVICE_STATE;
    }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    S->IntSignal = AVDTP_START;
    PnMutexRelease(&S->StreamMutex);
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_START, p, 1, &ec);
    switch (PnIntOutcome(AVDTP_START, status, AvdtpStateOpen, S->StreamState)) {
    case PnIntApply:
        SetStreamState(S, AvdtpStateStreaming, FALSE);
        status = STATUS_SUCCESS;
        break;
    case PnIntReached:
        /* both sides started: the receiver's START got there first (a BAD_STATE answer to
         * ours is that collision, not a failure) */
        if (!NT_SUCCESS(status)) PN_INFO("START crossed the receiver's (0x%08x, error 0x%02X)", status, ec);
        status = STATUS_SUCCESS;
        break;
    case PnIntLost:
        status = STATUS_INVALID_DEVICE_STATE;   /* suspended, closed or aborted meanwhile */
        break;
    default:
        break;
    }
    AvdtpTxnEnd(S);
    PnMutexRelease(&S->Mutex);
    return status;
}

NTSTATUS AvdtpSuspend(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState != AvdtpStateStreaming) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_SUCCESS; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    S->IntSignal = AVDTP_SUSPEND;
    PnMutexRelease(&S->StreamMutex);
    NTSTATUS status = AvdtpTransact(S, AVDTP_SUSPEND, p, 1, NULL);
    switch (PnIntOutcome(AVDTP_SUSPEND, status, AvdtpStateStreaming, S->StreamState)) {
    case PnIntApply:
        SetStreamState(S, AvdtpStateOpen, FALSE);
        status = STATUS_SUCCESS;
        break;
    case PnIntReached:
    case PnIntLost:
        status = STATUS_SUCCESS;   /* not streaming any more: the receiver suspended or closed it */
        break;
    default:
        break;
    }
    AvdtpTxnEnd(S);
    PnMutexRelease(&S->Mutex);
    return status;
}

NTSTATUS AvdtpClose(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    PN_CLOSE_ACTION action = PnStreamCloseAction(S->StreamState);
    if (action == PnCloseNothing) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_SUCCESS; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    UCHAR signal = 0;
    if (action == PnCloseSendClose) {
        SetStreamState(S, AvdtpStateClosing, FALSE);
        signal = AVDTP_CLOSE;
    } else if (action == PnCloseSendAbort) {
        /* Configured / Opening: CLOSE is not allowed, but the receiver must learn (T3) */
        SetStreamState(S, AvdtpStateAborting, FALSE);
        signal = AVDTP_ABORT;
    }
    AVDTP_STREAM_STATE sent = S->StreamState;   /* Closing / Aborting */
    S->IntSignal = signal;
    PnMutexRelease(&S->StreamMutex);
    NTSTATUS status = STATUS_SUCCESS;
    BOOLEAN teardown = TRUE;
    if (signal) {
        status = AvdtpTransact(S, signal, p, 1, NULL);
        teardown = PnIntOutcome(signal, status, sent, S->StreamState) != PnIntLost;
        if (teardown) KeClearEvent(&S->TransportIdle);   /* before the acceptor can judge against Closing */
        AvdtpTxnEnd(S);
    }
    if (teardown) AvdtpFinishTeardown(S, sent);
    PnMutexRelease(&S->Mutex);
    return status;
}

/* C4 (review round 3 #6): a command every sink answers in any stream state, for the
 * receiver's SEP of our stream. Only whether an answer (accept or reject) came matters;
 * nothing is parsed and no state changes. */
NTSTATUS AvdtpProbe(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    UCHAR seid = S->RemoteSeid;
    PnMutexRelease(&S->StreamMutex);
    if (!seid) { PnMutexRelease(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    UCHAR p[1] = { (UCHAR)(seid << 2) };
    UCHAR err;
    NTSTATUS status = AvdtpTransact(S, AVDTP_GET_CAPABILITIES, p, 1, &err);
    AvdtpTxnEnd(S);
    PnMutexRelease(&S->Mutex);
    return status;
}

NTSTATUS AvdtpAbort(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    PnMutexAcquire(&S->Mutex);
    PnMutexAcquire(&S->StreamMutex);
    if (S->StreamState == AvdtpStateIdle) { PnMutexRelease(&S->StreamMutex); PnMutexRelease(&S->Mutex); return STATUS_SUCCESS; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    SetStreamState(S, AvdtpStateAborting, FALSE);
    S->IntSignal = AVDTP_ABORT;
    PnMutexRelease(&S->StreamMutex);
    NTSTATUS status = AvdtpTransact(S, AVDTP_ABORT, p, 1, NULL);
    BOOLEAN teardown = PnIntOutcome(AVDTP_ABORT, status, AvdtpStateAborting, S->StreamState) != PnIntLost;
    if (teardown) KeClearEvent(&S->TransportIdle);   /* before the acceptor can judge against Aborting */
    AvdtpTxnEnd(S);
    if (teardown) AvdtpFinishTeardown(S, AvdtpStateAborting);
    PnMutexRelease(&S->Mutex);
    return status;
}
