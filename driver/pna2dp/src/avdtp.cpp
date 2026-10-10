/*
 * avdtp.cpp - AVDTP 1.3 signalling (initiator and acceptor) over PN L2CAP channels.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

#define AVDTP_EVT_SIG_DOWN     0x1
#define AVDTP_EVT_MEDIA_DOWN   0x2
#define AVDTP_EVT_MEDIA_UP     0x4
#define AVDTP_EVT_SIG_UP       0x8

static VOID AvdtpQueueEvent(_Inout_ PAVDTP_SESSION S, _In_ ULONG Bits);

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

static VOID SetStreamState(_Inout_ PAVDTP_SESSION S, _In_ AVDTP_STREAM_STATE New, _In_ BOOLEAN Remote)
{
    AVDTP_STREAM_STATE old;
    KIRQL irql;
    KeAcquireSpinLock(&S->Lock, &irql);
    old = S->StreamState;
    S->StreamState = New;
    KeReleaseSpinLock(&S->Lock, irql);
    if (old != New) {
        PN_INFO("AVDTP stream state %u -> %u (%s)", old, New, Remote ? "remote" : "local");
        if (S->Cb.OnStreamStateChanged) S->Cb.OnStreamStateChanged(S->Cb.Context, old, New, Remote);
    }
}

AVDTP_STREAM_STATE AvdtpGetState(_In_ PAVDTP_SESSION S) { return S->StreamState; }
PPN_L2CAP_CHANNEL AvdtpMediaChannel(_In_ PAVDTP_SESSION S) { return &S->Media; }
USHORT AvdtpMediaMtu(_In_ PAVDTP_SESSION S) { return S->Media.OutMtu ? S->Media.OutMtu : PN_L2CAP_SIG_MTU; }

ULONG AvdtpBuildMediaCodecCap(_Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap, _In_ UCHAR MediaCodecType,
                              _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len)
{
    if (Cap < 4 + Len) return 0;
    Out[0] = AVDTP_CAT_MEDIA_CODEC;
    Out[1] = (UCHAR)(2 + Len);
    Out[2] = (UCHAR)(AVDTP_MEDIA_TYPE_AUDIO << 4);
    Out[3] = MediaCodecType;
    RtlCopyMemory(Out + 4, Ies, Len);
    return 4 + Len;
}

/* Send one signalling message, fragmenting if it exceeds the channel MTU. */
static NTSTATUS AvdtpWriteMessage(_Inout_ PAVDTP_SESSION S, _In_ UCHAR MsgType, _In_ UCHAR Label, _In_ UCHAR SignalId,
                                  _In_reads_bytes_opt_(Len) const UCHAR *Payload, _In_ ULONG Len)
{
    PPN_L2CAP_CHANNEL ch = &S->Signalling;
    ULONG mtu = ch->OutMtu ? ch->OutMtu : 48;
    if (mtu < 8) mtu = 48;
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

static NTSTATUS AvdtpSendReject(_Inout_ PAVDTP_SESSION S, _In_ UCHAR Label, _In_ UCHAR SignalId, _In_ UCHAR ErrorCode)
{
    UCHAR p[1] = { ErrorCode };
    return AvdtpSendResponse(S, Label, SignalId, FALSE, p, 1);
}

/* INT: send a command and wait for the matching response. PASSIVE, under S->Mutex. */
static NTSTATUS AvdtpTransact(_Inout_ PAVDTP_SESSION S, _In_ UCHAR SignalId, _In_reads_bytes_opt_(Len) const UCHAR *Payload,
                              _In_ ULONG Len, _Out_opt_ PUCHAR ErrorCode)
{
    KIRQL irql;
    if (ErrorCode) *ErrorCode = 0;
    if (!S->SignallingUp) return STATUS_CONNECTION_DISCONNECTED;

    KeAcquireSpinLock(&S->Lock, &irql);
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
        return status;
    }
    LARGE_INTEGER timeout;
    timeout.QuadPart = -PN_HNS_PER_MS * AVDTP_RESPONSE_TIMEOUT_MS;
    status = KeWaitForSingleObject(&S->Txn.Done, Executive, KernelMode, FALSE, &timeout);
    KeAcquireSpinLock(&S->Lock, &irql);
    S->Txn.Active = FALSE;
    UCHAR type = S->Txn.MessageType;
    UCHAR err = S->Txn.ErrorCode;
    KeReleaseSpinLock(&S->Lock, irql);
    if (status == STATUS_TIMEOUT) {
        PN_WARN("AVDTP %s timed out", SigName(SignalId));
        return STATUS_IO_TIMEOUT;
    }
    if (!S->SignallingUp) return STATUS_CONNECTION_DISCONNECTED;
    if (type == AVDTP_MSG_RESPONSE_ACCEPT) return STATUS_SUCCESS;
    if (ErrorCode) *ErrorCode = err;
    if (type == AVDTP_MSG_GENERAL_REJECT) {
        PN_WARN("AVDTP %s: general reject", SigName(SignalId));
        return STATUS_NOT_SUPPORTED;
    }
    PN_WARN("AVDTP %s rejected, error 0x%02X", SigName(SignalId), err);
    return (err == AVDTP_ERR_NOT_SUPPORTED_COMMAND) ? STATUS_NOT_SUPPORTED : STATUS_DEVICE_PROTOCOL_ERROR;
}

/* ------------------------------------------------------------------------- */
/* Capability parsing                                                           */
/* ------------------------------------------------------------------------- */

static VOID ParseServiceCapabilities(_In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len, _Inout_ PAVDTP_REMOTE_SEP Sep)
{
    ULONG i = 0;
    Sep->ServiceCaps = 0;
    Sep->CapsValid = FALSE;
    while (i + 2 <= Len) {
        UCHAR cat = P[i], l = P[i + 1];
        const UCHAR *d = P + i + 2;
        if (i + 2 + l > Len) break;
        switch (cat) {
        case AVDTP_CAT_MEDIA_TRANSPORT: Sep->ServiceCaps |= PNA2DP_SVC_MEDIA_TRANSPORT; break;
        case AVDTP_CAT_REPORTING: Sep->ServiceCaps |= PNA2DP_SVC_REPORTING; break;
        case AVDTP_CAT_RECOVERY: Sep->ServiceCaps |= PNA2DP_SVC_RECOVERY; break;
        case AVDTP_CAT_CONTENT_PROTECTION: Sep->ServiceCaps |= PNA2DP_SVC_CONTENT_PROTECTION; break;
        case AVDTP_CAT_HEADER_COMPRESSION: Sep->ServiceCaps |= PNA2DP_SVC_HEADER_COMPRESSION; break;
        case AVDTP_CAT_MULTIPLEXING: Sep->ServiceCaps |= PNA2DP_SVC_MULTIPLEXING; break;
        case AVDTP_CAT_DELAY_REPORTING: Sep->ServiceCaps |= PNA2DP_SVC_DELAY_REPORTING; break;
        case AVDTP_CAT_MEDIA_CODEC:
            if (l >= 2) {
                Sep->MediaType = (UCHAR)(d[0] >> 4);
                Sep->MediaCodecType = d[1];
                Sep->CodecCapsLength = (UCHAR)PN_MIN(l - 2, sizeof(Sep->CodecCaps));
                RtlCopyMemory(Sep->CodecCaps, d + 2, Sep->CodecCapsLength);
                A2DP_CODEC_CAPS caps;
                if (A2dpParseCaps(Sep->MediaCodecType, Sep->CodecCaps, Sep->CodecCapsLength, &caps, &Sep->VendorId, &Sep->VendorCodecId)) {
                    Sep->Codec = caps.Codec;
                } else {
                    Sep->Codec = PNA2DP_CODEC_NONE;
                }
                Sep->CapsValid = TRUE;
            }
            break;
        default:
            break;
        }
        i += 2 + l;
    }
}

/* ------------------------------------------------------------------------- */
/* Receive path (DISPATCH): header parsing, reassembly, routing                 */
/* ------------------------------------------------------------------------- */

static VOID AvdtpHandleMessage(_Inout_ PAVDTP_SESSION S, _In_ UCHAR Label, _In_ UCHAR MsgType, _In_ UCHAR SignalId,
                               _In_reads_bytes_(Len) const UCHAR *Payload, _In_ ULONG Len)
{
    KIRQL irql;
    if (MsgType == AVDTP_MSG_COMMAND) {
        PAVDTP_ACP_ITEM item = (PAVDTP_ACP_ITEM)PnAllocNp(sizeof(*item), PNA2DP_TAG_AVDTP);
        if (!item) return;
        item->Label = Label;
        item->SignalId = SignalId;
        item->Length = PN_MIN(Len, AVDTP_MAX_SIGNAL_PAYLOAD);
        RtlCopyMemory(item->Payload, Payload, item->Length);
        KeAcquireSpinLock(&S->AcpQueueLock, &irql);
        InsertTailList(&S->AcpQueue, &item->Link);
        KeReleaseSpinLock(&S->AcpQueueLock, irql);
        if (InterlockedCompareExchange(&S->AcpWorkQueued, 1, 0) == 0) {
            IoQueueWorkItem(S->AcpWorkItem, [](PDEVICE_OBJECT, PVOID Ctx) {
                PAVDTP_SESSION s = (PAVDTP_SESSION)Ctx;
                extern VOID AvdtpAcpWorker(PAVDTP_SESSION);
                AvdtpAcpWorker(s);
            }, DelayedWorkQueue, S);
        }
        return;
    }
    /* response */
    KeAcquireSpinLock(&S->Lock, &irql);
    if (S->Txn.Active && S->Txn.Label == Label) {
        S->Txn.MessageType = MsgType;
        S->Txn.PayloadLength = PN_MIN(Len, AVDTP_MAX_SIGNAL_PAYLOAD);
        RtlCopyMemory(S->Txn.Payload, Payload, S->Txn.PayloadLength);
        S->Txn.ErrorCode = (MsgType == AVDTP_MSG_RESPONSE_REJECT && Len) ? Payload[Len - 1] : 0;
        /* SET_CONFIGURATION / RECONFIGURE rejects carry [category][error] */
        if (MsgType == AVDTP_MSG_RESPONSE_REJECT && Len >= 2 && (SignalId == AVDTP_SET_CONFIGURATION || SignalId == AVDTP_RECONFIGURE)) {
            S->Txn.ErrorCode = Payload[1];
        }
        KeReleaseSpinLock(&S->Lock, irql);
        KeSetEvent(&S->Txn.Done, 0, FALSE);
    } else {
        KeReleaseSpinLock(&S->Lock, irql);
        PN_WARN("AVDTP stray response label %u signal %s", Label, SigName(SignalId));
    }
}

static VOID AvdtpSignallingReceive(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (Len < 1) return;
    UCHAR label = Data[0] >> 4;
    UCHAR pkt = (Data[0] >> 2) & 3;
    UCHAR msgType = Data[0] & 3;
    AVDTP_REASSEMBLY *R = &S->Reasm;

    switch (pkt) {
    case AVDTP_PKT_SINGLE:
        if (Len < 2) return;
        AvdtpHandleMessage(S, label, msgType, (UCHAR)(Data[1] & 0x3F), Data + 2, Len - 2);
        break;
    case AVDTP_PKT_START:
        if (Len < 3) return;
        R->Active = TRUE;
        R->Label = label;
        R->MessageType = msgType;
        R->Nosp = Data[1];
        R->SignalId = (UCHAR)(Data[2] & 0x3F);
        R->Length = PN_MIN(Len - 3, AVDTP_MAX_SIGNAL_PAYLOAD);
        RtlCopyMemory(R->Buffer, Data + 3, R->Length);
        if (R->Nosp) R->Nosp--;
        break;
    case AVDTP_PKT_CONTINUE:
    case AVDTP_PKT_END:
        if (!R->Active || R->Label != label) { R->Active = FALSE; return; }
        {
            ULONG chunk = PN_MIN(Len - 1, AVDTP_MAX_SIGNAL_PAYLOAD - R->Length);
            RtlCopyMemory(R->Buffer + R->Length, Data + 1, chunk);
            R->Length += chunk;
        }
        if (R->Nosp) R->Nosp--;
        if (pkt == AVDTP_PKT_END) {
            R->Active = FALSE;
            AvdtpHandleMessage(S, R->Label, R->MessageType, R->SignalId, R->Buffer, R->Length);
        }
        break;
    }
}

static VOID AvdtpSignallingClosed(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_ NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    PN_INFO("signalling channel down 0x%08x", Status);
    S->SignallingUp = FALSE;
    S->DownStatus = Status;
    KeSetEvent(&S->Txn.Done, 0, FALSE);       /* unblock any waiter */
    AvdtpQueueEvent(S, AVDTP_EVT_SIG_DOWN);
}

static VOID AvdtpMediaReceive(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_reads_bytes_(Len) const UCHAR *Data, _In_ ULONG Len)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (S->Cb.OnMediaPacket) S->Cb.OnMediaPacket(S->Cb.Context, Data, Len);
}

static VOID AvdtpMediaClosed(_In_ PVOID Context, _In_ PPN_L2CAP_CHANNEL Channel, _In_ NTSTATUS Status)
{
    UNREFERENCED_PARAMETER(Channel);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    PN_INFO("media channel down 0x%08x", Status);
    S->MediaUp = FALSE;
    S->DownStatus = Status;
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

static LONG g_AvdtpPendingEventsDummy;   /* keeps the linker quiet if unused */

static VOID AvdtpEventWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PAVDTP_SESSION S = (PAVDTP_SESSION)Context;
    if (!S) return;
    for (;;) {
        LONG bits = InterlockedExchange(&S->DownQueued, 0);
        if (bits == 0) break;
        if (bits & AVDTP_EVT_MEDIA_UP) {
            SetStreamState(S, AvdtpStateOpen, TRUE);
        }
        if (bits & AVDTP_EVT_MEDIA_DOWN) {
            if (S->Cb.OnMediaDown) S->Cb.OnMediaDown(S->Cb.Context, S->DownStatus);
            if (S->StreamState == AvdtpStateStreaming || S->StreamState == AvdtpStateOpen || S->StreamState == AvdtpStateOpening) {
                SetStreamState(S, S->SignallingUp ? AvdtpStateConfigured : AvdtpStateIdle, TRUE);
            }
        }
        if (bits & AVDTP_EVT_SIG_DOWN) {
            if (S->Cb.OnSignallingDown) S->Cb.OnSignallingDown(S->Cb.Context, S->DownStatus);
        }
    }
}

static VOID AvdtpQueueEvent(_Inout_ PAVDTP_SESSION S, _In_ ULONG Bits)
{
    LONG old, next;
    do {
        old = S->DownQueued;
        next = old | (LONG)Bits;
    } while (InterlockedCompareExchange(&S->DownQueued, next, old) != old);
    if (old == 0 && S->DownWorkItem) {
        IoQueueWorkItem(S->DownWorkItem, AvdtpEventWorker, DelayedWorkQueue, S);
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

static ULONG BuildLocalCapabilities(_In_ PAVDTP_SESSION S, _In_ PAVDTP_LOCAL_SEP Sep, _In_ BOOLEAN All, _Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap)
{
    UNREFERENCED_PARAMETER(S);
    ULONG n = 0;
    if (Cap < 8) return 0;
    Out[n++] = AVDTP_CAT_MEDIA_TRANSPORT; Out[n++] = 0;
    UCHAR mct;
    UCHAR ies[32];
    ULONG l = A2dpBuildLocalCaps(Sep->Codec, ies, sizeof(ies), &mct);
    n += AvdtpBuildMediaCodecCap(Out + n, Cap - n, mct, ies, l);
    if (All && Sep->DelayReporting && n + 2 <= Cap) { Out[n++] = AVDTP_CAT_DELAY_REPORTING; Out[n++] = 0; }
    return n;
}

static VOID AvdtpProcessCommand(_Inout_ PAVDTP_SESSION S, _In_ PAVDTP_ACP_ITEM C)
{
    UCHAR rsp[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG n = 0;
    PN_VERB("AVDTP <- %s label %u len %u", SigName(C->SignalId), C->Label, C->Length);

    switch (C->SignalId) {
    case AVDTP_DISCOVER:
        for (ULONG i = 0; i < S->LocalSepCount && n + 2 <= sizeof(rsp); i++) {
            PAVDTP_LOCAL_SEP sep = &S->LocalSeps[i];
            BOOLEAN inUse = (S->StreamState != AvdtpStateIdle) && (S->LocalSeid == sep->Seid);
            rsp[n++] = (UCHAR)((sep->Seid << 2) | (inUse ? 0x02 : 0));
            rsp[n++] = (UCHAR)((sep->MediaType << 4) | (sep->Tsep << 3));
        }
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, rsp, n);
        break;

    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES: {
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        PAVDTP_LOCAL_SEP sep = FindLocalSep(S, (UCHAR)(C->Payload[0] >> 2));
        if (!sep) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_ACP_SEID); break; }
        n = BuildLocalCapabilities(S, sep, C->SignalId == AVDTP_GET_ALL_CAPABILITIES, rsp, sizeof(rsp));
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, rsp, n);
        break;
    }

    case AVDTP_SET_CONFIGURATION: {
        if (C->Length < 2) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        UCHAR acpSeid = (UCHAR)(C->Payload[0] >> 2), intSeid = (UCHAR)(C->Payload[1] >> 2);
        PAVDTP_LOCAL_SEP sep = FindLocalSep(S, acpSeid);
        if (!sep) { rsp[0] = 0; rsp[1] = AVDTP_ERR_BAD_ACP_SEID; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        if (S->StreamState != AvdtpStateIdle) { rsp[0] = 0; rsp[1] = AVDTP_ERR_SEP_IN_USE; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        /* parse requested service capabilities */
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        ParseServiceCapabilities(C->Payload + 2, C->Length - 2, &tmp);
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC;
        UCHAR err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        if (tmp.CapsValid && S->Cb.OnRemoteSetConfiguration) {
            err = S->Cb.OnRemoteSetConfiguration(S->Cb.Context, sep, intSeid, tmp.CodecCaps, tmp.CodecCapsLength, tmp.ServiceCaps, &failedCat);
        }
        if (err) { rsp[0] = failedCat; rsp[1] = err; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        S->LocalSeid = acpSeid;
        S->RemoteSeid = intSeid;
        S->ConfiguredIesLength = tmp.CodecCapsLength;
        RtlCopyMemory(S->ConfiguredIes, tmp.CodecCaps, tmp.CodecCapsLength);
        S->ConfiguredServiceCaps = tmp.ServiceCaps;
        S->DelayReportingNegotiated = (tmp.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) != 0;
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        SetStreamState(S, AvdtpStateConfigured, TRUE);
        break;
    }

    case AVDTP_GET_CONFIGURATION: {
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState == AvdtpStateIdle || (UCHAR)(C->Payload[0] >> 2) != S->LocalSeid) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_ACP_SEID); break; }
        PAVDTP_LOCAL_SEP sep = FindLocalSep(S, S->LocalSeid);
        UCHAR mct = A2DP_CODEC_SBC; UCHAR ies[32];
        A2dpBuildLocalCaps(sep ? sep->Codec : PNA2DP_CODEC_SBC, ies, sizeof(ies), &mct);
        rsp[n++] = AVDTP_CAT_MEDIA_TRANSPORT; rsp[n++] = 0;
        n += AvdtpBuildMediaCodecCap(rsp + n, sizeof(rsp) - n, mct, S->ConfiguredIes, S->ConfiguredIesLength);
        if (S->DelayReportingNegotiated) { rsp[n++] = AVDTP_CAT_DELAY_REPORTING; rsp[n++] = 0; }
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, rsp, n);
        break;
    }

    case AVDTP_RECONFIGURE: {
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen) { rsp[0] = 0; rsp[1] = AVDTP_ERR_BAD_STATE; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        ParseServiceCapabilities(C->Payload + 1, C->Length - 1, &tmp);
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC, err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        if (tmp.CapsValid && S->Cb.OnRemoteReconfigure) err = S->Cb.OnRemoteReconfigure(S->Cb.Context, tmp.CodecCaps, tmp.CodecCapsLength, &failedCat);
        if (err) { rsp[0] = failedCat; rsp[1] = err; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        S->ConfiguredIesLength = tmp.CodecCapsLength;
        RtlCopyMemory(S->ConfiguredIes, tmp.CodecCaps, tmp.CodecCapsLength);
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        break;
    }

    case AVDTP_OPEN:
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateConfigured) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_STATE); break; }
        S->MediaExpected = TRUE;
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        SetStreamState(S, AvdtpStateOpening, TRUE);
        break;

    case AVDTP_START:
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen) { rsp[0] = C->Payload[0]; rsp[1] = AVDTP_ERR_BAD_STATE; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        SetStreamState(S, AvdtpStateStreaming, TRUE);
        break;

    case AVDTP_SUSPEND:
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateStreaming) { rsp[0] = C->Payload[0]; rsp[1] = AVDTP_ERR_BAD_STATE; AvdtpSendResponse(S, C->Label, C->SignalId, FALSE, rsp, 2); break; }
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        SetStreamState(S, AvdtpStateOpen, TRUE);
        break;

    case AVDTP_CLOSE:
        if (C->Length < 1) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen && S->StreamState != AvdtpStateStreaming) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_STATE); break; }
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        SetStreamState(S, AvdtpStateClosing, TRUE);
        PnChannelDisconnectSync(&S->Media);
        S->MediaUp = FALSE;
        SetStreamState(S, AvdtpStateIdle, TRUE);
        break;

    case AVDTP_ABORT:
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        if (S->StreamState != AvdtpStateIdle) {
            SetStreamState(S, AvdtpStateAborting, TRUE);
            PnChannelDisconnectSync(&S->Media);
            S->MediaUp = FALSE;
            SetStreamState(S, AvdtpStateIdle, TRUE);
        }
        break;

    case AVDTP_DELAYREPORT: {
        if (C->Length < 3) { AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_BAD_LENGTH); break; }
        ULONG delay = ((ULONG)C->Payload[1] << 8) | C->Payload[2];
        S->SinkDelayTenthsMs = delay;
        AvdtpSendResponse(S, C->Label, C->SignalId, TRUE, NULL, 0);
        if (S->Cb.OnDelayReport) S->Cb.OnDelayReport(S->Cb.Context, delay);
        break;
    }

    case AVDTP_SECURITY_CONTROL:
        AvdtpSendReject(S, C->Label, C->SignalId, AVDTP_ERR_NOT_SUPPORTED_COMMAND);
        break;

    default:
        /* general reject for unknown signal identifiers */
        AvdtpWriteMessage(S, AVDTP_MSG_GENERAL_REJECT, C->Label, C->SignalId, NULL, 0);
        break;
    }
}

VOID AvdtpAcpWorker(PAVDTP_SESSION S)
{
    PAGED_CODE();
    for (;;) {
        KIRQL irql;
        PAVDTP_ACP_ITEM item = NULL;
        KeAcquireSpinLock(&S->AcpQueueLock, &irql);
        if (!IsListEmpty(&S->AcpQueue)) {
            PLIST_ENTRY e = RemoveHeadList(&S->AcpQueue);
            item = CONTAINING_RECORD(e, AVDTP_ACP_ITEM, Link);
        } else {
            InterlockedExchange(&S->AcpWorkQueued, 0);
        }
        KeReleaseSpinLock(&S->AcpQueueLock, irql);
        if (!item) break;
        ExAcquireFastMutex(&S->Mutex);
        if (S->SignallingUp) AvdtpProcessCommand(S, item);
        ExReleaseFastMutex(&S->Mutex);
        PnFree(item, PNA2DP_TAG_AVDTP);
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                    */
/* ------------------------------------------------------------------------- */

NTSTATUS AvdtpInitialize(_Out_ PAVDTP_SESSION S, _In_ PPN_BTH_LINK Link, _In_ PDEVICE_OBJECT DeviceObject, _In_ const AVDTP_CALLBACKS *Cb)
{
    PAGED_CODE();
    RtlZeroMemory(S, sizeof(*S));
    S->Link = Link;
    S->Cb = *Cb;
    S->DeviceObject = DeviceObject;
    ExInitializeFastMutex(&S->Mutex);
    KeInitializeSpinLock(&S->Lock);
    KeInitializeSpinLock(&S->AcpQueueLock);
    InitializeListHead(&S->AcpQueue);
    KeInitializeEvent(&S->Txn.Done, NotificationEvent, FALSE);
    S->RemoteVersion = 0x0103;
    S->AcpWorkItem = IoAllocateWorkItem(DeviceObject);
    S->DownWorkItem = IoAllocateWorkItem(DeviceObject);
    if (!S->AcpWorkItem || !S->DownWorkItem) return STATUS_INSUFFICIENT_RESOURCES;

    NTSTATUS status = PnChannelInitialize(&S->Signalling, Link, FALSE, PN_L2CAP_SIG_MTU, PN_L2CAP_SIG_MTU,
                                          AvdtpSignallingReceive, AvdtpSignallingClosed, NULL, S);
    if (!NT_SUCCESS(status)) return status;
    status = PnChannelInitialize(&S->Media, Link, TRUE, 2048, 2048, AvdtpMediaReceive, AvdtpMediaClosed, AvdtpMediaWriteDone, S);
    if (!NT_SUCCESS(status)) return status;
    UNREFERENCED_PARAMETER(g_AvdtpPendingEventsDummy);
    return STATUS_SUCCESS;
}

VOID AvdtpUninitialize(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (!S->Link) return;
    AvdtpDisconnect(S);
    PnChannelUninitialize(&S->Media);
    PnChannelUninitialize(&S->Signalling);
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

NTSTATUS AvdtpConnectSignalling(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    if (S->SignallingUp) return STATUS_SUCCESS;
    NTSTATUS status = PnChannelConnect(&S->Signalling, PSM_AVDTP, PN_L2CAP_SIG_MTU, 10000);
    if (!NT_SUCCESS(status)) {
        PN_WARN("AVDTP signalling connect failed 0x%08x", status);
        return status;
    }
    S->SignallingUp = TRUE;
    S->RemoteSignallingInitiated = FALSE;
    S->Reasm.Active = FALSE;
    PnChannelStartReaders(&S->Signalling);
    return STATUS_SUCCESS;
}

NTSTATUS AvdtpAcceptIncoming(_Inout_ PAVDTP_SESSION S, _In_ PINDICATION_PARAMETERS Params)
{
    NTSTATUS status;
    if (!S->SignallingUp && S->Signalling.State != PnChannelConnecting) {
        S->RemoteSignallingInitiated = TRUE;
        S->Reasm.Active = FALSE;
        status = PnChannelAccept(&S->Signalling, Params, PN_L2CAP_SIG_MTU);
        if (NT_SUCCESS(status)) {
            /* mark up optimistically; readers start when the accept completes (device work item) */
            S->SignallingUp = TRUE;
            AvdtpQueueEvent(S, AVDTP_EVT_SIG_UP);
        }
        return status;
    }
    if (S->SignallingUp && !S->MediaUp && S->Media.State != PnChannelConnecting &&
        (S->MediaExpected || S->StreamState == AvdtpStateConfigured || S->StreamState == AvdtpStateOpening)) {
        status = PnChannelAccept(&S->Media, Params, PN_L2CAP_MEDIA_MTU_PREF);
        if (NT_SUCCESS(status)) {
            S->MediaExpected = FALSE;
            S->MediaUp = TRUE;
            AvdtpQueueEvent(S, AVDTP_EVT_MEDIA_UP);
        }
        return status;
    }
    PN_WARN("unexpected incoming L2CAP connection (sig %u media %u state %u)", S->SignallingUp, S->MediaUp, S->StreamState);
    return PnLinkRejectIncoming(S->Link, Params);
}

/* Called by the device layer after an accepted channel completes (SIG_UP / MEDIA_UP events). */
extern "C" VOID AvdtpOnAcceptedChannelReady(_Inout_ PAVDTP_SESSION S, _In_ BOOLEAN Media)
{
    PPN_L2CAP_CHANNEL ch = Media ? &S->Media : &S->Signalling;
    KeWaitForSingleObject(&ch->ConnectedEvent, Executive, KernelMode, FALSE, NULL);
    if (NT_SUCCESS(ch->OpenStatus)) {
        PnChannelStartReaders(ch);
    } else {
        if (Media) S->MediaUp = FALSE; else S->SignallingUp = FALSE;
    }
}

VOID AvdtpDisconnect(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateClosing, FALSE);
    PnChannelDisconnectSync(&S->Media);
    S->MediaUp = FALSE;
    S->MediaExpected = FALSE;
    PnChannelDisconnectSync(&S->Signalling);
    S->SignallingUp = FALSE;
    S->RemoteSepsValid = FALSE;
    S->RemoteSepCount = 0;
    S->Reasm.Active = FALSE;
    KeSetEvent(&S->Txn.Done, 0, FALSE);
    if (S->StreamState != AvdtpStateIdle) SetStreamState(S, AvdtpStateIdle, FALSE);
    ExReleaseFastMutex(&S->Mutex);
}

/* ------------------------------------------------------------------------- */
/* INT procedures                                                               */
/* ------------------------------------------------------------------------- */

NTSTATUS AvdtpDiscover(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    NTSTATUS status;
    UCHAR err;
    ExAcquireFastMutex(&S->Mutex);
    S->RemoteSepCount = 0;
    S->RemoteSepsValid = FALSE;
    status = AvdtpTransact(S, AVDTP_DISCOVER, NULL, 0, &err);
    if (!NT_SUCCESS(status)) { ExReleaseFastMutex(&S->Mutex); return status; }
    ULONG count = S->Txn.PayloadLength / 2;
    for (ULONG i = 0; i < count && S->RemoteSepCount < AVDTP_MAX_SEPS; i++) {
        PAVDTP_REMOTE_SEP sep = &S->RemoteSeps[S->RemoteSepCount];
        RtlZeroMemory(sep, sizeof(*sep));
        sep->Seid = (UCHAR)(S->Txn.Payload[i * 2] >> 2);
        sep->InUse = (UCHAR)((S->Txn.Payload[i * 2] >> 1) & 1);
        sep->MediaType = (UCHAR)(S->Txn.Payload[i * 2 + 1] >> 4);
        sep->Tsep = (UCHAR)((S->Txn.Payload[i * 2 + 1] >> 3) & 1);
        if (sep->MediaType != AVDTP_MEDIA_TYPE_AUDIO) continue;
        S->RemoteSepCount++;
    }
    PN_INFO("AVDTP discovered %u audio SEPs", S->RemoteSepCount);
    BOOLEAN useAll = (S->RemoteVersion >= 0x0103);
    for (ULONG i = 0; i < S->RemoteSepCount; i++) {
        PAVDTP_REMOTE_SEP sep = &S->RemoteSeps[i];
        UCHAR p[1] = { (UCHAR)(sep->Seid << 2) };
        status = AvdtpTransact(S, useAll ? AVDTP_GET_ALL_CAPABILITIES : AVDTP_GET_CAPABILITIES, p, 1, &err);
        if (status == STATUS_NOT_SUPPORTED && useAll) {
            useAll = FALSE;
            S->RemoteVersion = 0x0102;
            status = AvdtpTransact(S, AVDTP_GET_CAPABILITIES, p, 1, &err);
        }
        if (!NT_SUCCESS(status)) {
            PN_WARN("GET_CAPABILITIES seid %u failed 0x%08x", sep->Seid, status);
            if (status == STATUS_IO_TIMEOUT || status == STATUS_CONNECTION_DISCONNECTED) break;
            continue;
        }
        ParseServiceCapabilities(S->Txn.Payload, S->Txn.PayloadLength, sep);
        PN_INFO("  SEP %u: %s in-use %u codec %s (type 0x%02X vid %08X cid %04X) svc 0x%02X", sep->Seid,
                sep->Tsep ? "SNK" : "SRC", sep->InUse, A2dpCodecName(sep->Codec), sep->MediaCodecType, sep->VendorId,
                sep->VendorCodecId, sep->ServiceCaps);
    }
    S->RemoteSepsValid = NT_SUCCESS(status) || S->RemoteSepCount > 0;
    ExReleaseFastMutex(&S->Mutex);
    return S->RemoteSepsValid ? STATUS_SUCCESS : status;
}

NTSTATUS AvdtpSetConfiguration(_Inout_ PAVDTP_SESSION S, _In_ UCHAR LocalSeid, _In_ UCHAR RemoteSeid,
                               _In_reads_bytes_(Len) const UCHAR *CodecIes, _In_ ULONG Len,
                               _In_ BOOLEAN RequestDelayReporting, _In_ BOOLEAN RequestContentProtection)
{
    PAGED_CODE();
    UCHAR p[64];
    ULONG n = 0, err = 0;
    UCHAR mct;
    UCHAR dummy[32];
    PAVDTP_LOCAL_SEP local = FindLocalSep(S, LocalSeid);
    if (!local) return STATUS_INVALID_PARAMETER;
    A2dpBuildLocalCaps(local->Codec, dummy, sizeof(dummy), &mct);

    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState != AvdtpStateIdle) { ExReleaseFastMutex(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    p[n++] = (UCHAR)(RemoteSeid << 2);
    p[n++] = (UCHAR)(LocalSeid << 2);
    p[n++] = AVDTP_CAT_MEDIA_TRANSPORT; p[n++] = 0;
    n += AvdtpBuildMediaCodecCap(p + n, sizeof(p) - n, mct, CodecIes, Len);
    if (RequestContentProtection && n + 4 <= sizeof(p)) { p[n++] = AVDTP_CAT_CONTENT_PROTECTION; p[n++] = 2; p[n++] = 0x02; p[n++] = 0x00; } /* SCMS-T */
    if (RequestDelayReporting && n + 2 <= sizeof(p)) { p[n++] = AVDTP_CAT_DELAY_REPORTING; p[n++] = 0; }
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_SET_CONFIGURATION, p, n, &ec);
    err = ec;
    if (NT_SUCCESS(status)) {
        S->LocalSeid = LocalSeid;
        S->RemoteSeid = RemoteSeid;
        S->ConfiguredIesLength = (UCHAR)PN_MIN(Len, sizeof(S->ConfiguredIes));
        RtlCopyMemory(S->ConfiguredIes, CodecIes, S->ConfiguredIesLength);
        S->DelayReportingNegotiated = RequestDelayReporting;
        SetStreamState(S, AvdtpStateConfigured, FALSE);
    } else {
        PN_WARN("SET_CONFIGURATION failed 0x%08x (avdtp error 0x%02X)", status, err);
    }
    ExReleaseFastMutex(&S->Mutex);
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
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState != AvdtpStateOpen) { ExReleaseFastMutex(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    p[n++] = (UCHAR)(S->RemoteSeid << 2);
    n += AvdtpBuildMediaCodecCap(p + n, sizeof(p) - n, mct, CodecIes, Len);
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_RECONFIGURE, p, n, &ec);
    if (NT_SUCCESS(status)) {
        S->ConfiguredIesLength = (UCHAR)PN_MIN(Len, sizeof(S->ConfiguredIes));
        RtlCopyMemory(S->ConfiguredIes, CodecIes, S->ConfiguredIesLength);
    }
    ExReleaseFastMutex(&S->Mutex);
    return status;
}

NTSTATUS AvdtpOpen(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState != AvdtpStateConfigured) { ExReleaseFastMutex(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    NTSTATUS status = AvdtpTransact(S, AVDTP_OPEN, p, 1, NULL);
    if (!NT_SUCCESS(status)) { ExReleaseFastMutex(&S->Mutex); return status; }
    SetStreamState(S, AvdtpStateOpening, FALSE);
    /* INT opens the transport channel */
    status = PnChannelConnect(&S->Media, PSM_AVDTP, PN_L2CAP_MEDIA_MTU_PREF, 10000);
    if (!NT_SUCCESS(status)) {
        PN_WARN("media channel connect failed 0x%08x; aborting stream", status);
        UCHAR a[1] = { (UCHAR)(S->RemoteSeid << 2) };
        AvdtpTransact(S, AVDTP_ABORT, a, 1, NULL);
        SetStreamState(S, AvdtpStateIdle, FALSE);
        ExReleaseFastMutex(&S->Mutex);
        return status;
    }
    S->MediaUp = TRUE;
    PnChannelStartReaders(&S->Media);
    SetStreamState(S, AvdtpStateOpen, FALSE);
    ExReleaseFastMutex(&S->Mutex);
    return STATUS_SUCCESS;
}

NTSTATUS AvdtpStart(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState == AvdtpStateStreaming) { ExReleaseFastMutex(&S->Mutex); return STATUS_SUCCESS; }
    if (S->StreamState != AvdtpStateOpen) { ExReleaseFastMutex(&S->Mutex); return STATUS_INVALID_DEVICE_STATE; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    UCHAR ec = 0;
    NTSTATUS status = AvdtpTransact(S, AVDTP_START, p, 1, &ec);
    if (NT_SUCCESS(status) || (status == STATUS_DEVICE_PROTOCOL_ERROR && ec == AVDTP_ERR_BAD_STATE && S->StreamState == AvdtpStateStreaming)) {
        SetStreamState(S, AvdtpStateStreaming, FALSE);
        status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&S->Mutex);
    return status;
}

NTSTATUS AvdtpSuspend(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState != AvdtpStateStreaming) { ExReleaseFastMutex(&S->Mutex); return STATUS_SUCCESS; }
    UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
    NTSTATUS status = AvdtpTransact(S, AVDTP_SUSPEND, p, 1, NULL);
    if (NT_SUCCESS(status) || status == STATUS_DEVICE_PROTOCOL_ERROR) {
        SetStreamState(S, AvdtpStateOpen, FALSE);
        status = STATUS_SUCCESS;
    }
    ExReleaseFastMutex(&S->Mutex);
    return status;
}

NTSTATUS AvdtpClose(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    if (S->StreamState == AvdtpStateIdle) { ExReleaseFastMutex(&S->Mutex); return STATUS_SUCCESS; }
    NTSTATUS status = STATUS_SUCCESS;
    if (S->StreamState == AvdtpStateOpen || S->StreamState == AvdtpStateStreaming) {
        UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
        SetStreamState(S, AvdtpStateClosing, FALSE);
        status = AvdtpTransact(S, AVDTP_CLOSE, p, 1, NULL);
    }
    PnChannelDisconnectSync(&S->Media);
    S->MediaUp = FALSE;
    S->MediaExpected = FALSE;
    SetStreamState(S, AvdtpStateIdle, FALSE);
    ExReleaseFastMutex(&S->Mutex);
    return status;
}

NTSTATUS AvdtpAbort(_Inout_ PAVDTP_SESSION S)
{
    PAGED_CODE();
    ExAcquireFastMutex(&S->Mutex);
    NTSTATUS status = STATUS_SUCCESS;
    if (S->StreamState != AvdtpStateIdle) {
        UCHAR p[1] = { (UCHAR)(S->RemoteSeid << 2) };
        SetStreamState(S, AvdtpStateAborting, FALSE);
        status = AvdtpTransact(S, AVDTP_ABORT, p, 1, NULL);
        PnChannelDisconnectSync(&S->Media);
        S->MediaUp = FALSE;
        S->MediaExpected = FALSE;
        SetStreamState(S, AvdtpStateIdle, FALSE);
    }
    ExReleaseFastMutex(&S->Mutex);
    return status;
}
