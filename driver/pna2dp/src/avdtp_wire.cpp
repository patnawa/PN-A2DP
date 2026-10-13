/* AVDTP wire validation, without I/O or allocation. SPDX-License-Identifier: MIT */
#include "codec_core.h"
#include "avdtp_wire.h"

AVDTP_PARSE_RESULT AvdtpReassemble(AVDTP_REASSEMBLY *R, const UCHAR *Data, ULONG Len)
{
    if (!Len) { R->Active = FALSE; return AvdtpInvalid; }
    UCHAR label = Data[0] >> 4, packet = (Data[0] >> 2) & 3, message = Data[0] & 3;
    ULONG header = packet == AVDTP_PKT_SINGLE ? 2 : packet == AVDTP_PKT_START ? 3 : 1;
    if (Len < header || Len - header > sizeof(R->Buffer)) { R->Active = FALSE; return AvdtpInvalid; }
    if (packet == AVDTP_PKT_SINGLE || packet == AVDTP_PKT_START) {
        R->Active = FALSE;
        if (packet == AVDTP_PKT_START && Data[1] < 2) return AvdtpInvalid;
        R->Label = label;
        R->MessageType = message;
        R->SignalId = Data[header - 1] & 0x3f;
        R->Length = Len - header;
        RtlCopyMemory(R->Buffer, Data + header, R->Length);
        if (packet == AVDTP_PKT_SINGLE) return AvdtpComplete;
        R->Nosp = Data[1] - 1;
        R->Active = TRUE;
        return AvdtpMore;
    }
    if (!R->Active || R->Label != label || R->MessageType != message ||
        !R->Nosp || R->Length > sizeof(R->Buffer) || Len - 1 > sizeof(R->Buffer) - R->Length ||
        (packet == AVDTP_PKT_END) != (R->Nosp == 1)) {
        R->Active = FALSE;
        return AvdtpInvalid;
    }
    RtlCopyMemory(R->Buffer + R->Length, Data + 1, Len - 1);
    R->Length += Len - 1;
    --R->Nosp;
    if (packet == AVDTP_PKT_END) { R->Active = FALSE; return AvdtpComplete; }
    return AvdtpMore;
}

/* Lenient for what a receiver advertises (T7): several Content Protection entries are
 * recorded as a type mask, categories this AVDTP version does not know are skipped when
 * their length is valid. Strict acceptance of configurations is in AvdtpAcpProcess. */
BOOLEAN AvdtpParseServiceCapabilities(const UCHAR *P, ULONG Len, PAVDTP_REMOTE_SEP Sep)
{
    ULONG seen = 0;
    Sep->CapsValid = FALSE;
    Sep->ServiceCaps = 0;
    Sep->Codec = PNA2DP_CODEC_NONE;
    Sep->CpTypeMask = 0;
    Sep->UnknownCategory = 0;
    for (ULONG i = 0; i < Len;) {
        if (Len - i < 2) return FALSE;
        UCHAR cat = P[i], size = P[i + 1];
        const UCHAR *d = P + i + 2;
        if (size > Len - i - 2 || !cat) return FALSE;
        if (cat > AVDTP_CAT_DELAY_REPORTING) {
            if (!Sep->UnknownCategory) Sep->UnknownCategory = cat;
            i += 2 + size;
            continue;
        }
        if ((seen & (1u << cat)) && cat != AVDTP_CAT_CONTENT_PROTECTION) return FALSE;
        seen |= 1u << cat;
        switch (cat) {
        case AVDTP_CAT_MEDIA_TRANSPORT:
        case AVDTP_CAT_REPORTING:
        case AVDTP_CAT_DELAY_REPORTING:
            if (size) return FALSE;
            break;
        case AVDTP_CAT_RECOVERY: if (size != 3) return FALSE; break;
        case AVDTP_CAT_CONTENT_PROTECTION: {
            if (size < 2) return FALSE;
            USHORT type = (USHORT)(d[0] | ((USHORT)d[1] << 8));   /* CP_TYPE_LSB, CP_TYPE_MSB */
            Sep->CpTypeMask |= type == AVDTP_CP_TYPE_DTCP ? AVDTP_CP_MASK_DTCP :
                               type == AVDTP_CP_TYPE_SCMS_T ? AVDTP_CP_MASK_SCMS_T : AVDTP_CP_MASK_OTHER;
            break;
        }
        case AVDTP_CAT_HEADER_COMPRESSION: if (size != 1) return FALSE; break;
        case AVDTP_CAT_MULTIPLEXING: if (size < 1) return FALSE; break;
        case AVDTP_CAT_MEDIA_CODEC: {
            if (size < 2 || size - 2 > sizeof(Sep->CodecCaps) || d[0] >> 4 != AVDTP_MEDIA_TYPE_AUDIO) return FALSE;
            Sep->MediaType = d[0] >> 4;
            Sep->MediaCodecType = d[1];
            Sep->CodecCapsLength = size - 2;
            RtlCopyMemory(Sep->CodecCaps, d + 2, Sep->CodecCapsLength);
            A2DP_CODEC_CAPS caps;
            if (A2dpParseCaps(Sep->MediaCodecType, Sep->CodecCaps, Sep->CodecCapsLength, &caps, &Sep->VendorId, &Sep->VendorCodecId))
                Sep->Codec = caps.Codec;
            break;
        }
        }
        if (cat == AVDTP_CAT_DELAY_REPORTING) Sep->ServiceCaps |= PNA2DP_SVC_DELAY_REPORTING;
        else if (cat != AVDTP_CAT_MEDIA_CODEC) Sep->ServiceCaps |= 1u << (cat - 1);
        i += 2 + size;
    }
    Sep->CapsValid = (seen & (1u << AVDTP_CAT_MEDIA_CODEC)) != 0;
    return Sep->CapsValid;
}

ULONG AvdtpParseDiscover(const UCHAR *P, ULONG Len, PAVDTP_REMOTE_SEP Seps, ULONG Max)
{
    ULONG n = 0;
    for (ULONG i = 0; i < Len / 2 && n < Max; i++) {
        PAVDTP_REMOTE_SEP sep = &Seps[n];
        RtlZeroMemory(sep, sizeof(*sep));
        sep->Seid = (UCHAR)(P[i * 2] >> 2);
        sep->InUse = (UCHAR)((P[i * 2] >> 1) & 1);
        sep->MediaType = (UCHAR)(P[i * 2 + 1] >> 4);
        sep->Tsep = (UCHAR)((P[i * 2 + 1] >> 3) & 1);
        if (sep->MediaType != AVDTP_MEDIA_TYPE_AUDIO) continue;
        n++;
    }
    return n;
}

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

/* ------------------------------------------------------------------------- */
/* Acceptor command handling                                                    */
/* ------------------------------------------------------------------------- */

static PAVDTP_LOCAL_SEP AcpFindLocalSep(_In_ const AVDTP_ACP_CONTEXT *C, _In_ UCHAR Seid)
{
    for (ULONG i = 0; i < C->LocalSepCount; i++) if (C->LocalSeps[i].Seid == Seid) return &C->LocalSeps[i];
    return NULL;
}

static ULONG AcpBuildLocalCapabilities(_In_ const AVDTP_LOCAL_SEP *Sep, _In_ BOOLEAN All, _Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap)
{
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

static VOID AcpReply(_Inout_ AVDTP_ACP_RESULT *Out, _In_ BOOLEAN Accept, _In_ ULONG Length)
{
    Out->MessageType = Accept ? AVDTP_MSG_RESPONSE_ACCEPT : AVDTP_MSG_RESPONSE_REJECT;
    Out->ResponseLength = Length;
}

static VOID AcpReject1(_Inout_ AVDTP_ACP_RESULT *Out, _In_ UCHAR Error)
{
    Out->Response[0] = Error;
    AcpReply(Out, FALSE, 1);
}

/* SET_CONFIGURATION / RECONFIGURE / START / SUSPEND rejects: [category or SEID][error] */
static VOID AcpReject2(_Inout_ AVDTP_ACP_RESULT *Out, _In_ UCHAR First, _In_ UCHAR Error)
{
    Out->Response[0] = First;
    Out->Response[1] = Error;
    AcpReply(Out, FALSE, 2);
}

static VOID AcpNewState(_Inout_ AVDTP_ACP_RESULT *Out, _In_ AVDTP_STREAM_STATE State)
{
    Out->Effects |= AVDTP_ACP_FX_NEW_STATE;
    Out->NewState = State;
}

/* Service categories of a SET_CONFIGURATION / RECONFIGURE request (T7). We support Media
 * Transport, Media Codec and Delay Reporting, and no content protection (SCMS-T is not
 * implemented, so accepting it would stream without the header it requires). Rejects
 * carry [category][error] (AVDTP 8.9.3, 8.11.3). RECONFIGURE may only change Media Codec
 * and Content Protection (AVDTP 8.11.1). Structural errors are left to the capability
 * parser. Returns 0 when the categories are acceptable. */
static UCHAR AcpCheckRequestedCategories(_In_reads_bytes_(Len) const UCHAR *P, _In_ ULONG Len, _In_ BOOLEAN Reconfigure,
                                         _Out_ PUCHAR FailedCategory)
{
    BOOLEAN transport = FALSE;
    *FailedCategory = 0;
    for (ULONG i = 0; i + 2 <= Len;) {
        UCHAR cat = P[i], size = P[i + 1];
        if (size > Len - i - 2) break;
        *FailedCategory = cat;
        switch (cat) {
        case AVDTP_CAT_MEDIA_CODEC:
            break;
        case AVDTP_CAT_MEDIA_TRANSPORT:
        case AVDTP_CAT_DELAY_REPORTING:
            if (Reconfigure) return AVDTP_ERR_INVALID_CAPABILITIES;
            if (cat == AVDTP_CAT_MEDIA_TRANSPORT) transport = TRUE;
            break;
        case AVDTP_CAT_CONTENT_PROTECTION:
            return A2DP_ERR_INVALID_CP_TYPE;
        case AVDTP_CAT_REPORTING:
        case AVDTP_CAT_RECOVERY:
        case AVDTP_CAT_HEADER_COMPRESSION:
        case AVDTP_CAT_MULTIPLEXING:
            return Reconfigure ? AVDTP_ERR_INVALID_CAPABILITIES : AVDTP_ERR_BAD_SERV_CATEGORY;
        default:
            return AVDTP_ERR_BAD_SERV_CATEGORY;
        }
        i += 2 + size;
    }
    if (!Reconfigure && !transport) { *FailedCategory = AVDTP_CAT_MEDIA_TRANSPORT; return AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT; }
    *FailedCategory = 0;
    return 0;
}

VOID AvdtpAcpProcess(_In_ const AVDTP_ACP_CONTEXT *C, _In_ UCHAR SignalId, _In_reads_bytes_(Len) const UCHAR *Payload,
                     _In_ ULONG Len, _Out_ AVDTP_ACP_RESULT *Out)
{
    PUCHAR rsp = Out->Response;
    ULONG n = 0;
    Out->MessageType = AVDTP_MSG_RESPONSE_ACCEPT;
    Out->ResponseLength = 0;
    Out->Effects = 0;
    Out->MidState = Out->NewState = C->StreamState;
    Out->ConfigLocalSeid = Out->ConfigRemoteSeid = 0;
    Out->IesLength = 0;
    Out->ServiceCaps = 0;
    Out->DelayReporting = FALSE;
    Out->DelayTenthsMs = 0;

    switch (SignalId) {
    case AVDTP_DISCOVER:
        for (ULONG i = 0; i < C->LocalSepCount && n + 2 <= sizeof(Out->Response); i++) {
            const AVDTP_LOCAL_SEP *sep = &C->LocalSeps[i];
            /* in use: our stream, or a codec that is off or has no encoder now (T10) */
            BOOLEAN inUse = ((C->StreamState != AvdtpStateIdle) && (C->LocalSeid == sep->Seid)) || sep->InUse;
            rsp[n++] = (UCHAR)((sep->Seid << 2) | (inUse ? 0x02 : 0));
            rsp[n++] = (UCHAR)((sep->MediaType << 4) | (sep->Tsep << 3));
        }
        AcpReply(Out, TRUE, n);
        break;

    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES: {
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        PAVDTP_LOCAL_SEP sep = AcpFindLocalSep(C, (UCHAR)(Payload[0] >> 2));
        if (!sep) { AcpReject1(Out, AVDTP_ERR_BAD_ACP_SEID); break; }
        AcpReply(Out, TRUE, AcpBuildLocalCapabilities(sep, SignalId == AVDTP_GET_ALL_CAPABILITIES, rsp, sizeof(Out->Response)));
        break;
    }

    case AVDTP_SET_CONFIGURATION: {
        if (Len < 2) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        UCHAR acpSeid = (UCHAR)(Payload[0] >> 2), intSeid = (UCHAR)(Payload[1] >> 2);
        PAVDTP_LOCAL_SEP sep = AcpFindLocalSep(C, acpSeid);
        if (!sep) { AcpReject2(Out, 0, AVDTP_ERR_BAD_ACP_SEID); break; }
        /* one stream per device: ours, or the one our outstanding SET_CONFIGURATION sets up (T9) */
        if (C->StreamState != AvdtpStateIdle || C->IntSignal == AVDTP_SET_CONFIGURATION) { AcpReject2(Out, 0, AVDTP_ERR_SEP_IN_USE); break; }
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC;
        UCHAR err = AcpCheckRequestedCategories(Payload + 2, Len - 2, FALSE, &failedCat);
        if (err) { AcpReject2(Out, failedCat, err); break; }
        /* parse requested service capabilities */
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        AvdtpParseServiceCapabilities(Payload + 2, Len - 2, &tmp);
        failedCat = AVDTP_CAT_MEDIA_CODEC;
        err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        if (tmp.CapsValid && tmp.Codec == sep->Codec && C->CheckConfiguration) {
            err = C->CheckConfiguration(C->CheckContext, sep, intSeid, tmp.CodecCaps, tmp.CodecCapsLength, tmp.ServiceCaps, &failedCat);
        }
        if (err) { AcpReject2(Out, failedCat, err); break; }
        /* a new configuration: the last delay report belonged to another stream (T6) */
        Out->Effects |= AVDTP_ACP_FX_STORE_CONFIG | AVDTP_ACP_FX_RESET_DELAY;
        Out->ConfigLocalSeid = acpSeid;
        Out->ConfigRemoteSeid = intSeid;
        Out->IesLength = tmp.CodecCapsLength;
        RtlCopyMemory(Out->Ies, tmp.CodecCaps, tmp.CodecCapsLength);
        Out->ServiceCaps = tmp.ServiceCaps;
        Out->DelayReporting = (tmp.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) != 0;
        AcpReply(Out, TRUE, 0);
        AcpNewState(Out, AvdtpStateConfigured);
        break;
    }

    case AVDTP_GET_CONFIGURATION: {
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState == AvdtpStateIdle || (UCHAR)(Payload[0] >> 2) != C->LocalSeid) { AcpReject1(Out, AVDTP_ERR_BAD_ACP_SEID); break; }
        PAVDTP_LOCAL_SEP sep = AcpFindLocalSep(C, C->LocalSeid);
        UCHAR mct = A2DP_CODEC_SBC; UCHAR ies[32];
        A2dpBuildLocalCaps(sep ? sep->Codec : PNA2DP_CODEC_SBC, ies, sizeof(ies), &mct);
        rsp[n++] = AVDTP_CAT_MEDIA_TRANSPORT; rsp[n++] = 0;
        n += AvdtpBuildMediaCodecCap(rsp + n, sizeof(Out->Response) - n, mct, C->ConfiguredIes, C->ConfiguredIesLength);
        if (C->DelayReportingNegotiated) { rsp[n++] = AVDTP_CAT_DELAY_REPORTING; rsp[n++] = 0; }
        AcpReply(Out, TRUE, n);
        break;
    }

    case AVDTP_RECONFIGURE: {
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState != AvdtpStateOpen) { AcpReject2(Out, 0, AVDTP_ERR_BAD_STATE); break; }
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC;
        UCHAR err = AcpCheckRequestedCategories(Payload + 1, Len - 1, TRUE, &failedCat);
        if (err) { AcpReject2(Out, failedCat, err); break; }
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        AvdtpParseServiceCapabilities(Payload + 1, Len - 1, &tmp);
        failedCat = AVDTP_CAT_MEDIA_CODEC;
        err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        PAVDTP_LOCAL_SEP local = AcpFindLocalSep(C, C->LocalSeid);
        if (tmp.CapsValid && local && tmp.Codec == local->Codec && C->CheckReconfigure)
            err = C->CheckReconfigure(C->CheckContext, tmp.CodecCaps, tmp.CodecCapsLength, &failedCat);
        if (err) { AcpReject2(Out, failedCat, err); break; }
        Out->Effects |= AVDTP_ACP_FX_STORE_IES;
        Out->IesLength = tmp.CodecCapsLength;
        RtlCopyMemory(Out->Ies, tmp.CodecCaps, tmp.CodecCapsLength);
        AcpReply(Out, TRUE, 0);
        break;
    }

    case AVDTP_OPEN:
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState != AvdtpStateConfigured) { AcpReject1(Out, AVDTP_ERR_BAD_STATE); break; }
        /* both sides OPENing: ours wins, so exactly one side connects the transport (T9);
         * our OPEN learns of the collision from this (review #3) */
        if (C->IntSignal == AVDTP_OPEN) { Out->Effects |= AVDTP_ACP_FX_OPEN_COLLISION; AcpReject1(Out, AVDTP_ERR_BAD_STATE); break; }
        Out->Effects |= AVDTP_ACP_FX_MEDIA_EXPECTED;
        AcpReply(Out, TRUE, 0);
        AcpNewState(Out, AvdtpStateOpening);
        break;

    case AVDTP_START:
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState != AvdtpStateOpen) { AcpReject2(Out, Payload[0], AVDTP_ERR_BAD_STATE); break; }
        Out->Effects |= AVDTP_ACP_FX_REMOTE_START;
        AcpReply(Out, TRUE, 0);
        AcpNewState(Out, AvdtpStateStreaming);
        break;

    case AVDTP_SUSPEND:
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState != AvdtpStateStreaming) { AcpReject2(Out, Payload[0], AVDTP_ERR_BAD_STATE); break; }
        Out->Effects |= AVDTP_ACP_FX_REMOTE_SUSPEND;
        AcpReply(Out, TRUE, 0);
        AcpNewState(Out, AvdtpStateOpen);
        break;

    case AVDTP_CLOSE:
        if (Len < 1) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        if (C->StreamState != AvdtpStateOpen && C->StreamState != AvdtpStateStreaming) { AcpReject1(Out, AVDTP_ERR_BAD_STATE); break; }
        AcpReply(Out, TRUE, 0);
        Out->Effects |= AVDTP_ACP_FX_MID_STATE | AVDTP_ACP_FX_DISCONNECT_MEDIA | AVDTP_ACP_FX_RELEASE_REMOTE;
        Out->MidState = AvdtpStateClosing;
        AcpNewState(Out, AvdtpStateIdle);
        break;

    case AVDTP_ABORT:
        AcpReply(Out, TRUE, 0);
        if (C->StreamState != AvdtpStateIdle) {
            Out->Effects |= AVDTP_ACP_FX_MID_STATE | AVDTP_ACP_FX_DISCONNECT_MEDIA | AVDTP_ACP_FX_RELEASE_REMOTE;
            Out->MidState = AvdtpStateAborting;
            AcpNewState(Out, AvdtpStateIdle);
        }
        break;

    case AVDTP_DELAYREPORT:
        if (Len < 3) { AcpReject1(Out, AVDTP_ERR_BAD_LENGTH); break; }
        /* only for the SEP of a configured stream (T6) */
        if (C->StreamState == AvdtpStateIdle || C->StreamState == AvdtpStateClosing || C->StreamState == AvdtpStateAborting) {
            AcpReject1(Out, AVDTP_ERR_BAD_STATE);
            break;
        }
        if ((UCHAR)(Payload[0] >> 2) != C->LocalSeid) { AcpReject1(Out, AVDTP_ERR_BAD_ACP_SEID); break; }
        Out->DelayTenthsMs = ((ULONG)Payload[1] << 8) | Payload[2];
        Out->Effects |= AVDTP_ACP_FX_STORE_DELAY | AVDTP_ACP_FX_DELAY_CALLBACK;
        AcpReply(Out, TRUE, 0);
        break;

    case AVDTP_SECURITY_CONTROL:
        AcpReject1(Out, AVDTP_ERR_NOT_SUPPORTED_COMMAND);
        break;

    default:
        /* general reject for unknown signal identifiers */
        Out->MessageType = AVDTP_MSG_GENERAL_REJECT;
        Out->ResponseLength = 0;
        break;
    }
}
