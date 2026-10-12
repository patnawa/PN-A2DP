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

BOOLEAN AvdtpParseServiceCapabilities(const UCHAR *P, ULONG Len, PAVDTP_REMOTE_SEP Sep)
{
    ULONG seen = 0;
    Sep->CapsValid = FALSE;
    Sep->ServiceCaps = 0;
    Sep->Codec = PNA2DP_CODEC_NONE;
    for (ULONG i = 0; i < Len;) {
        if (Len - i < 2) return FALSE;
        UCHAR cat = P[i], size = P[i + 1];
        const UCHAR *d = P + i + 2;
        if (size > Len - i - 2 || !cat || cat > AVDTP_CAT_DELAY_REPORTING || (seen & (1u << cat))) return FALSE;
        seen |= 1u << cat;
        switch (cat) {
        case AVDTP_CAT_MEDIA_TRANSPORT:
        case AVDTP_CAT_REPORTING:
        case AVDTP_CAT_DELAY_REPORTING:
            if (size) return FALSE;
            break;
        case AVDTP_CAT_RECOVERY: if (size != 3) return FALSE; break;
        case AVDTP_CAT_CONTENT_PROTECTION: if (size < 2) return FALSE; break;
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
