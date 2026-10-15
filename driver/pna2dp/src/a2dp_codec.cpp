/*
 * a2dp_codec.cpp - codec capability parsing, selection and configuration building.
 * SPDX-License-Identifier: MIT
 */
#include "codec_core.h"
#include "audio_policy.h"

static const struct { ULONG Mask; ULONG Hz; } g_RateTable[] = {
    { PNA2DP_RATE_8000, 8000 }, { PNA2DP_RATE_11025, 11025 }, { PNA2DP_RATE_12000, 12000 }, { PNA2DP_RATE_16000, 16000 },
    { PNA2DP_RATE_22050, 22050 }, { PNA2DP_RATE_24000, 24000 }, { PNA2DP_RATE_32000, 32000 }, { PNA2DP_RATE_44100, 44100 },
    { PNA2DP_RATE_48000, 48000 }, { PNA2DP_RATE_64000, 64000 }, { PNA2DP_RATE_88200, 88200 }, { PNA2DP_RATE_96000, 96000 },
};

ULONG A2dpRateMaskToHz(_In_ ULONG SingleBit)
{
    for (ULONG i = 0; i < PN_ARRAYSIZE(g_RateTable); i++) if (g_RateTable[i].Mask == SingleBit) return g_RateTable[i].Hz;
    return 0;
}

ULONG A2dpHzToRateMask(_In_ ULONG Hz)
{
    for (ULONG i = 0; i < PN_ARRAYSIZE(g_RateTable); i++) if (g_RateTable[i].Hz == Hz) return g_RateTable[i].Mask;
    return 0;
}

const char *A2dpCodecName(_In_ ULONG Codec)
{
    switch (Codec) {
    case PNA2DP_CODEC_SBC: return "SBC";
    case PNA2DP_CODEC_AAC: return "AAC";
    case PNA2DP_CODEC_APTX: return "aptX";
    case PNA2DP_CODEC_APTX_HD: return "aptX HD";
    case PNA2DP_CODEC_APTX_LL: return "aptX LL";
    case PNA2DP_CODEC_LDAC: return "LDAC";
    case PNA2DP_CODEC_FASTSTREAM: return "FastStream";
    case PNA2DP_CODEC_OPUS: return "Opus";
    case PNA2DP_CODEC_LC3PLUS: return "LC3plus";
    default: return "none";
    }
}

static ULONG ReadLe32(const UCHAR *p) { return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) | ((ULONG)p[3] << 24); }
static USHORT ReadLe16(const UCHAR *p) { return (USHORT)(p[0] | (p[1] << 8)); }
static VOID WriteLe32(UCHAR *p, ULONG v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); p[2] = (UCHAR)(v >> 16); p[3] = (UCHAR)(v >> 24); }
static VOID WriteLe16(UCHAR *p, USHORT v) { p[0] = (UCHAR)v; p[1] = (UCHAR)(v >> 8); }
static BOOLEAN SingleBit(ULONG v) { return v && !(v & (v - 1)); }

/* ---- SBC ---------------------------------------------------------------- */

static ULONG SbcRateBitsToMask(UCHAR b)
{
    ULONG m = 0;
    if (b & PNA2DP_SBC_FREQ_16000) m |= PNA2DP_RATE_16000;
    if (b & PNA2DP_SBC_FREQ_32000) m |= PNA2DP_RATE_32000;
    if (b & PNA2DP_SBC_FREQ_44100) m |= PNA2DP_RATE_44100;
    if (b & PNA2DP_SBC_FREQ_48000) m |= PNA2DP_RATE_48000;
    return m;
}
static UCHAR SbcRateMaskToBits(ULONG m)
{
    UCHAR b = 0;
    if (m & PNA2DP_RATE_16000) b |= PNA2DP_SBC_FREQ_16000;
    if (m & PNA2DP_RATE_32000) b |= PNA2DP_SBC_FREQ_32000;
    if (m & PNA2DP_RATE_44100) b |= PNA2DP_SBC_FREQ_44100;
    if (m & PNA2DP_RATE_48000) b |= PNA2DP_SBC_FREQ_48000;
    return b;
}
static ULONG SbcChBitsToMask(UCHAR b)
{
    ULONG m = 0;
    if (b & PNA2DP_SBC_CH_MONO) m |= PNA2DP_CHMODE_MONO;
    if (b & PNA2DP_SBC_CH_DUAL) m |= PNA2DP_CHMODE_DUAL;
    if (b & PNA2DP_SBC_CH_STEREO) m |= PNA2DP_CHMODE_STEREO;
    if (b & PNA2DP_SBC_CH_JOINT) m |= PNA2DP_CHMODE_JOINT;
    return m;
}
static UCHAR SbcChMaskToBits(ULONG m)
{
    UCHAR b = 0;
    if (m & PNA2DP_CHMODE_MONO) b |= PNA2DP_SBC_CH_MONO;
    if (m & PNA2DP_CHMODE_DUAL) b |= PNA2DP_SBC_CH_DUAL;
    if (m & PNA2DP_CHMODE_STEREO) b |= PNA2DP_SBC_CH_STEREO;
    if (m & PNA2DP_CHMODE_JOINT) b |= PNA2DP_SBC_CH_JOINT;
    return b;
}

/* ---- aptX family -------------------------------------------------------- */

static ULONG AptxRateBitsToMask(UCHAR b)
{
    ULONG m = 0;
    if (b & APTX_FREQ_16000) m |= PNA2DP_RATE_16000;
    if (b & APTX_FREQ_32000) m |= PNA2DP_RATE_32000;
    if (b & APTX_FREQ_44100) m |= PNA2DP_RATE_44100;
    if (b & APTX_FREQ_48000) m |= PNA2DP_RATE_48000;
    return m;
}
static UCHAR AptxRateMaskToBits(ULONG m)
{
    UCHAR b = 0;
    if (m & PNA2DP_RATE_16000) b |= APTX_FREQ_16000;
    if (m & PNA2DP_RATE_32000) b |= APTX_FREQ_32000;
    if (m & PNA2DP_RATE_44100) b |= APTX_FREQ_44100;
    if (m & PNA2DP_RATE_48000) b |= APTX_FREQ_48000;
    return b;
}

/* ---- AAC ---------------------------------------------------------------- */

static ULONG AacRateBitsToMask(UCHAR b1, UCHAR b2)
{
    ULONG m = 0;
    if (b1 & 0x80) m |= PNA2DP_RATE_8000;
    if (b1 & 0x40) m |= PNA2DP_RATE_11025;
    if (b1 & 0x20) m |= PNA2DP_RATE_12000;
    if (b1 & 0x10) m |= PNA2DP_RATE_16000;
    if (b1 & 0x08) m |= PNA2DP_RATE_22050;
    if (b1 & 0x04) m |= PNA2DP_RATE_24000;
    if (b1 & 0x02) m |= PNA2DP_RATE_32000;
    if (b1 & 0x01) m |= PNA2DP_RATE_44100;
    if (b2 & 0x80) m |= PNA2DP_RATE_48000;
    if (b2 & 0x40) m |= PNA2DP_RATE_64000;
    if (b2 & 0x20) m |= PNA2DP_RATE_88200;
    if (b2 & 0x10) m |= PNA2DP_RATE_96000;
    return m;
}
static VOID AacRateMaskToBits(ULONG m, UCHAR *b1, UCHAR *b2)
{
    UCHAR x = 0, y = 0;
    if (m & PNA2DP_RATE_8000) x |= 0x80;
    if (m & PNA2DP_RATE_11025) x |= 0x40;
    if (m & PNA2DP_RATE_12000) x |= 0x20;
    if (m & PNA2DP_RATE_16000) x |= 0x10;
    if (m & PNA2DP_RATE_22050) x |= 0x08;
    if (m & PNA2DP_RATE_24000) x |= 0x04;
    if (m & PNA2DP_RATE_32000) x |= 0x02;
    if (m & PNA2DP_RATE_44100) x |= 0x01;
    if (m & PNA2DP_RATE_48000) y |= 0x80;
    if (m & PNA2DP_RATE_64000) y |= 0x40;
    if (m & PNA2DP_RATE_88200) y |= 0x20;
    if (m & PNA2DP_RATE_96000) y |= 0x10;
    *b1 = x; *b2 = y;
}

/* ---- LDAC --------------------------------------------------------------- */

static ULONG LdacRateBitsToMask(UCHAR b)
{
    ULONG m = 0;
    if (b & LDAC_FREQ_44100) m |= PNA2DP_RATE_44100;
    if (b & LDAC_FREQ_48000) m |= PNA2DP_RATE_48000;
    if (b & LDAC_FREQ_88200) m |= PNA2DP_RATE_88200;
    if (b & LDAC_FREQ_96000) m |= PNA2DP_RATE_96000;
    return m;
}
static UCHAR LdacRateMaskToBits(ULONG m)
{
    UCHAR b = 0;
    if (m & PNA2DP_RATE_44100) b |= LDAC_FREQ_44100;
    if (m & PNA2DP_RATE_48000) b |= LDAC_FREQ_48000;
    if (m & PNA2DP_RATE_88200) b |= LDAC_FREQ_88200;
    if (m & PNA2DP_RATE_96000) b |= LDAC_FREQ_96000;
    return b;
}

/* ------------------------------------------------------------------------- */

BOOLEAN A2dpParseCaps(_In_ UCHAR MediaCodecType, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                      _Out_ PA2DP_CODEC_CAPS Caps, _Out_opt_ PULONG VendorId, _Out_opt_ PUSHORT VendorCodecId)
{
    RtlZeroMemory(Caps, sizeof(*Caps));
    if (VendorId) *VendorId = 0;
    if (VendorCodecId) *VendorCodecId = 0;

    switch (MediaCodecType) {
    case A2DP_CODEC_SBC:
        if (Len != 4) return FALSE;
        Caps->Codec = PNA2DP_CODEC_SBC;
        Caps->RateMask = SbcRateBitsToMask(Ies[0] & 0xF0);
        Caps->ChannelModeMask = SbcChBitsToMask(Ies[0] & 0x0F);
        Caps->SbcBlockMask = Ies[1] & 0xF0;
        Caps->SbcSubbandMask = Ies[1] & 0x0C;
        Caps->SbcAllocMask = Ies[1] & 0x03;
        Caps->SbcMinBitpool = Ies[2];
        Caps->SbcMaxBitpool = Ies[3];
        Caps->FormatMask = PNA2DP_FMT_S16;
        return Caps->RateMask && Caps->ChannelModeMask && Caps->SbcBlockMask && Caps->SbcSubbandMask &&
               Caps->SbcAllocMask && Caps->SbcMinBitpool >= 2 && Caps->SbcMaxBitpool <= 250 &&
               Caps->SbcMinBitpool <= Caps->SbcMaxBitpool;

    case A2DP_CODEC_MPEG24_AAC:
        if (Len != 6) return FALSE;
        Caps->Codec = PNA2DP_CODEC_AAC;
        Caps->AacObjectMask = Ies[0];
        Caps->RateMask = AacRateBitsToMask(Ies[1], Ies[2]);
        Caps->ChannelModeMask = ((Ies[2] & AAC_CH_MONO) ? PNA2DP_CHMODE_MONO : 0) | ((Ies[2] & AAC_CH_STEREO) ? PNA2DP_CHMODE_STEREO : 0);
        Caps->AacVbr = (Ies[3] & 0x80) != 0;
        Caps->AacMaxBitrate = ((ULONG)(Ies[3] & 0x7F) << 16) | ((ULONG)Ies[4] << 8) | Ies[5];
        Caps->FormatMask = PNA2DP_FMT_S16 | PNA2DP_FMT_S24 | PNA2DP_FMT_S32;
        return Caps->RateMask && Caps->ChannelModeMask && Caps->AacObjectMask;

    case A2DP_CODEC_VENDOR: {
        if (Len < 6) return FALSE;
        ULONG vid = ReadLe32(Ies);
        USHORT cid = ReadLe16(Ies + 4);
        const UCHAR *v = Ies + 6;
        ULONG vlen = Len - 6;
        if (VendorId) *VendorId = vid;
        if (VendorCodecId) *VendorCodecId = cid;
        if (vid == A2DP_VENDOR_APTX && cid == A2DP_CODECID_APTX) {
            if (vlen < 1) return FALSE;
            Caps->Codec = PNA2DP_CODEC_APTX;
            Caps->RateMask = AptxRateBitsToMask(v[0] & 0xF0);
            Caps->ChannelModeMask = ((v[0] & APTX_CH_STEREO) ? PNA2DP_CHMODE_STEREO : 0) | ((v[0] & APTX_CH_MONO) ? PNA2DP_CHMODE_MONO : 0);
            Caps->FormatMask = PNA2DP_FMT_S24;
            return TRUE;
        }
        if (vid == A2DP_VENDOR_APTX_HD && cid == A2DP_CODECID_APTX_HD) {
            if (vlen < 5) return FALSE;
            Caps->Codec = PNA2DP_CODEC_APTX_HD;
            Caps->RateMask = AptxRateBitsToMask(v[0] & 0xF0);
            Caps->ChannelModeMask = ((v[0] & APTX_CH_STEREO) ? PNA2DP_CHMODE_STEREO : 0) | ((v[0] & APTX_CH_MONO) ? PNA2DP_CHMODE_MONO : 0);
            Caps->FormatMask = PNA2DP_FMT_S24;
            return TRUE;
        }
        if (vid == A2DP_VENDOR_CSR && cid == A2DP_CODECID_APTX_LL) {
            if (vlen < 2) return FALSE;
            Caps->Codec = PNA2DP_CODEC_APTX_LL;
            Caps->RateMask = AptxRateBitsToMask(v[0] & 0xF0);
            Caps->ChannelModeMask = ((v[0] & APTX_CH_STEREO) ? PNA2DP_CHMODE_STEREO : 0) | ((v[0] & APTX_CH_MONO) ? PNA2DP_CHMODE_MONO : 0);
            Caps->AptxLlBidirectional = (v[1] & 0x01) != 0;
            Caps->FormatMask = PNA2DP_FMT_S24;
            return TRUE;
        }
        if (vid == A2DP_VENDOR_SONY && cid == A2DP_CODECID_LDAC) {
            if (vlen < 2) return FALSE;
            Caps->Codec = PNA2DP_CODEC_LDAC;
            Caps->RateMask = LdacRateBitsToMask(v[0]);
            Caps->ChannelModeMask = ((v[1] & LDAC_CH_MONO) ? PNA2DP_CHMODE_MONO : 0) | ((v[1] & LDAC_CH_DUAL) ? PNA2DP_CHMODE_DUAL : 0) |
                                    ((v[1] & LDAC_CH_STEREO) ? PNA2DP_CHMODE_STEREO : 0);
            Caps->FormatMask = PNA2DP_FMT_S16 | PNA2DP_FMT_S24 | PNA2DP_FMT_S32 | PNA2DP_FMT_F32;
            return TRUE;
        }
        if (vid == A2DP_VENDOR_CSR && cid == A2DP_CODECID_FASTSTREAM) {
            if (vlen < 2) return FALSE;
            Caps->Codec = PNA2DP_CODEC_FASTSTREAM;
            Caps->FsDirections = v[0];
            Caps->RateMask = ((v[1] & FASTSTREAM_SINK_FREQ_48000) ? PNA2DP_RATE_48000 : 0) | ((v[1] & FASTSTREAM_SINK_FREQ_44100) ? PNA2DP_RATE_44100 : 0);
            Caps->ChannelModeMask = PNA2DP_CHMODE_JOINT;
            Caps->FormatMask = PNA2DP_FMT_S16;
            return TRUE;
        }
        if (vid == A2DP_VENDOR_LINUXFOUND && cid == A2DP_CODECID_OPUS) {
            /* PipeWire OPUS-A2DP 0.5: octet0 = channel mask (bit0 mono, bit1 stereo...), later bytes frame durations/bitrate */
            if (vlen < 1) return FALSE;
            Caps->Codec = PNA2DP_CODEC_OPUS;
            Caps->RateMask = PNA2DP_RATE_48000;
            Caps->ChannelModeMask = ((v[0] & 0x01) ? PNA2DP_CHMODE_MONO : 0) | ((v[0] & 0x02) ? PNA2DP_CHMODE_STEREO : 0);
            Caps->FormatMask = PNA2DP_FMT_S16 | PNA2DP_FMT_F32;
            return TRUE;
        }
        return FALSE;
    }
    default:
        return FALSE;
    }
}

BOOLEAN A2dpParseConfig(_In_ UCHAR MediaCodecType, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len, _Out_ PA2DP_CODEC_CONFIG Cfg)
{
    A2DP_CODEC_CAPS caps;
    ULONG vid; USHORT cid;
    RtlZeroMemory(Cfg, sizeof(*Cfg));
    if (!A2dpParseCaps(MediaCodecType, Ies, Len, &caps, &vid, &cid)) return FALSE;
    if (Len > sizeof(Cfg->Ies) || !SingleBit(caps.RateMask) || !SingleBit(caps.ChannelModeMask)) return FALSE;
    Cfg->Codec = caps.Codec;
    Cfg->MediaCodecType = MediaCodecType;
    Cfg->SampleRate = A2dpRateMaskToHz(caps.RateMask);
    Cfg->ChannelMode = caps.ChannelModeMask;
    Cfg->Channels = (Cfg->ChannelMode == PNA2DP_CHMODE_MONO) ? 1 : 2;
    /* a configuration without a usable rate or channel mode is invalid (e.g. LDAC 176.4/192 k) */
    if (!Cfg->SampleRate || !Cfg->ChannelMode) return FALSE;
    switch (caps.Codec) {
    case PNA2DP_CODEC_SBC:
        if (!SingleBit(caps.SbcBlockMask) || !SingleBit(caps.SbcSubbandMask) || !SingleBit(caps.SbcAllocMask)) return FALSE;
        Cfg->SbcBlocks = (caps.SbcBlockMask & PNA2DP_SBC_BLOCKS_16) ? 16 : (caps.SbcBlockMask & PNA2DP_SBC_BLOCKS_12) ? 12 :
                         (caps.SbcBlockMask & PNA2DP_SBC_BLOCKS_8) ? 8 : 4;
        Cfg->SbcSubbands = (caps.SbcSubbandMask & PNA2DP_SBC_SUBBANDS_8) ? 8 : 4;
        Cfg->SbcAlloc = (caps.SbcAllocMask & PNA2DP_SBC_ALLOC_LOUDNESS) ? 0 : 1;
        Cfg->SbcMinBitpool = caps.SbcMinBitpool;
        Cfg->SbcMaxBitpool = caps.SbcMaxBitpool;
        Cfg->SbcBitpool = caps.SbcMaxBitpool;
        if (Cfg->SbcMaxBitpool > PN_MIN(250u, Cfg->SbcSubbands *
            ((Cfg->ChannelMode == PNA2DP_CHMODE_MONO || Cfg->ChannelMode == PNA2DP_CHMODE_DUAL) ? 16u : 32u))) return FALSE;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    case PNA2DP_CODEC_AAC:
        if (!SingleBit(caps.AacObjectMask) || !(caps.AacObjectMask & (AAC_OBJ_MPEG2_LC | AAC_OBJ_MPEG4_LC))) return FALSE;
        Cfg->AacObjectMask = caps.AacObjectMask;
        Cfg->AacObjectType = (caps.AacObjectMask & AAC_OBJ_MPEG2_LC) ? 2 : (caps.AacObjectMask & AAC_OBJ_MPEG4_LC) ? 2 :
                             (caps.AacObjectMask & AAC_OBJ_HE_V1) ? 5 : 29;
        Cfg->AacVbr = caps.AacVbr;
        Cfg->BitrateBps = caps.AacMaxBitrate;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    case PNA2DP_CODEC_LDAC:
        Cfg->PcmFormat = PNA2DP_FMT_S32;
        Cfg->LdacEqmid = PNA2DP_LDAC_EQMID_SQ;
        break;
    case PNA2DP_CODEC_APTX_HD:
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
        Cfg->PcmFormat = PNA2DP_FMT_S24;
        break;
    default:
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    }
    Cfg->IesLength = (UCHAR)PN_MIN(Len, sizeof(Cfg->Ies));
    RtlCopyMemory(Cfg->Ies, Ies, Cfg->IesLength);
    Cfg->MaxBitrateBps = Cfg->BitrateBps = A2dpNominalBitrate(Cfg);
    return TRUE;
}

ULONG A2dpBuildLocalCaps(_In_ ULONG Codec, _Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap, _Out_ PUCHAR MediaCodecType)
{
    *MediaCodecType = A2DP_CODEC_VENDOR;
    if (Cap < 16) return 0;
    RtlZeroMemory(Out, Cap);
    switch (Codec) {
    case PNA2DP_CODEC_SBC:
        *MediaCodecType = A2DP_CODEC_SBC;
        Out[0] = PNA2DP_SBC_FREQ_16000 | PNA2DP_SBC_FREQ_32000 | PNA2DP_SBC_FREQ_44100 | PNA2DP_SBC_FREQ_48000 |
                 PNA2DP_SBC_CH_MONO | PNA2DP_SBC_CH_DUAL | PNA2DP_SBC_CH_STEREO | PNA2DP_SBC_CH_JOINT;
        Out[1] = PNA2DP_SBC_BLOCKS_4 | PNA2DP_SBC_BLOCKS_8 | PNA2DP_SBC_BLOCKS_12 | PNA2DP_SBC_BLOCKS_16 |
                 PNA2DP_SBC_SUBBANDS_4 | PNA2DP_SBC_SUBBANDS_8 | PNA2DP_SBC_ALLOC_SNR | PNA2DP_SBC_ALLOC_LOUDNESS;
        Out[2] = 2;
        Out[3] = 250;
        return 4;
    case PNA2DP_CODEC_AAC: {
        UCHAR b1, b2;
        *MediaCodecType = A2DP_CODEC_MPEG24_AAC;
        Out[0] = AAC_OBJ_MPEG2_LC | AAC_OBJ_MPEG4_LC;
        AacRateMaskToBits(PNA2DP_RATE_44100 | PNA2DP_RATE_48000 | PNA2DP_RATE_88200 | PNA2DP_RATE_96000, &b1, &b2);
        Out[1] = b1;
        Out[2] = (UCHAR)(b2 | AAC_CH_MONO | AAC_CH_STEREO);
        Out[3] = 0x80 | 0x04;   /* VBR + bitrate 0x04E200 = 320000 */
        Out[4] = 0xE2;
        Out[5] = 0x00;          /* 0x04E200 = 320000 */
        return 6;
    }
    case PNA2DP_CODEC_APTX:
        *MediaCodecType = A2DP_CODEC_VENDOR;
        WriteLe32(Out, A2DP_VENDOR_APTX); WriteLe16(Out + 4, A2DP_CODECID_APTX);
        Out[6] = APTX_FREQ_44100 | APTX_FREQ_48000 | APTX_CH_STEREO;
        return 7;
    case PNA2DP_CODEC_APTX_HD:
        *MediaCodecType = A2DP_CODEC_VENDOR;
        WriteLe32(Out, A2DP_VENDOR_APTX_HD); WriteLe16(Out + 4, A2DP_CODECID_APTX_HD);
        Out[6] = APTX_FREQ_44100 | APTX_FREQ_48000 | APTX_CH_STEREO;
        return 11;   /* 4 reserved bytes */
    case PNA2DP_CODEC_APTX_LL:
        *MediaCodecType = A2DP_CODEC_VENDOR;
        WriteLe32(Out, A2DP_VENDOR_CSR); WriteLe16(Out + 4, A2DP_CODECID_APTX_LL);
        Out[6] = APTX_FREQ_44100 | APTX_FREQ_48000 | APTX_CH_STEREO;
        Out[7] = 0x00;  /* no bidirectional link */
        return 8;
    case PNA2DP_CODEC_LDAC:
        *MediaCodecType = A2DP_CODEC_VENDOR;
        WriteLe32(Out, A2DP_VENDOR_SONY); WriteLe16(Out + 4, A2DP_CODECID_LDAC);
        Out[6] = LDAC_FREQ_44100 | LDAC_FREQ_48000 | LDAC_FREQ_88200 | LDAC_FREQ_96000;
        Out[7] = LDAC_CH_MONO | LDAC_CH_DUAL | LDAC_CH_STEREO;
        return 8;
    default:
        return 0;
    }
}

static ULONG PickRate(ULONG Allowed, ULONG Preferred)
{
    if (Allowed == 0) return 0;
    ULONG pm = A2dpHzToRateMask(Preferred);
    if (pm & Allowed) return Preferred;
    /* nearest higher first, then highest available */
    for (ULONG i = 0; i < PN_ARRAYSIZE(g_RateTable); i++) {
        if ((g_RateTable[i].Mask & Allowed) && g_RateTable[i].Hz >= Preferred) return g_RateTable[i].Hz;
    }
    for (LONG i = (LONG)PN_ARRAYSIZE(g_RateTable) - 1; i >= 0; i--) {
        if (g_RateTable[i].Mask & Allowed) return g_RateTable[i].Hz;
    }
    return 0;
}

static ULONG PickChannelMode(ULONG Allowed, ULONG PreferDual)
{
    if (PreferDual && (Allowed & PNA2DP_CHMODE_DUAL)) return PNA2DP_CHMODE_DUAL;
    if (Allowed & PNA2DP_CHMODE_JOINT) return PNA2DP_CHMODE_JOINT;
    if (Allowed & PNA2DP_CHMODE_STEREO) return PNA2DP_CHMODE_STEREO;
    if (Allowed & PNA2DP_CHMODE_DUAL) return PNA2DP_CHMODE_DUAL;
    if (Allowed & PNA2DP_CHMODE_MONO) return PNA2DP_CHMODE_MONO;
    return 0;
}

BOOLEAN A2dpSelectConfig(_In_ const A2DP_CODEC_CAPS *RemoteCaps, _In_ const PNA2DP_CONFIG *Pref, _In_ ULONG PreferredRate,
                         _In_ ULONG PreferredBits, _Out_ PA2DP_CODEC_CONFIG Cfg)
{
    RtlZeroMemory(Cfg, sizeof(*Cfg));
    UCHAR localIe[32], localType;
    A2DP_CODEC_CAPS local, common = *RemoteCaps;
    ULONG localLen = A2dpBuildLocalCaps(common.Codec, localIe, sizeof(localIe), &localType);
    if (!localLen || !A2dpParseCaps(localType, localIe, localLen, &local, NULL, NULL)) return FALSE;
    common.RateMask &= local.RateMask;
    common.ChannelModeMask &= local.ChannelModeMask;
    common.AacObjectMask &= local.AacObjectMask;
    const A2DP_CODEC_CAPS *Caps = &common;
    Cfg->Codec = Caps->Codec;
    ULONG rates = Caps->RateMask;
    ULONG chmodes = Caps->ChannelModeMask;
    if (!rates || !chmodes) return FALSE;
    BOOLEAN preferDual = (Pref->Flags & PNA2DP_CFG_DUAL_CHANNEL_PREFER) != 0;
    /* SBC XQ: dual channel, 8 subbands, 16 blocks; the pump packs it to the radio */
    BOOLEAN sbcXq = (Pref->Flags & PNA2DP_CFG_SBC_XQ) != 0 && Caps->Codec == PNA2DP_CODEC_SBC &&
        (Caps->ChannelModeMask & PNA2DP_CHMODE_DUAL) && (Caps->SbcSubbandMask & PNA2DP_SBC_SUBBANDS_8) &&
        (Caps->SbcBlockMask & PNA2DP_SBC_BLOCKS_16);

    switch (Caps->Codec) {
    case PNA2DP_CODEC_SBC: {
        if (Pref->Sbc.RateMask) rates &= SbcRateBitsToMask(Pref->Sbc.RateMask);
        if (Pref->Sbc.ChannelModeMask) chmodes &= SbcChBitsToMask(Pref->Sbc.ChannelModeMask);
        if (!rates) rates = Caps->RateMask;
        if (!chmodes) chmodes = Caps->ChannelModeMask;
        if (sbcXq) chmodes = PNA2DP_CHMODE_DUAL;
        Cfg->MediaCodecType = A2DP_CODEC_SBC;
        Cfg->SampleRate = PickRate(rates, PreferredRate);
        Cfg->ChannelMode = PickChannelMode(chmodes, preferDual);
        UCHAR blocks = Caps->SbcBlockMask, subs = Caps->SbcSubbandMask, alloc = Caps->SbcAllocMask;
        if (sbcXq) { blocks = PNA2DP_SBC_BLOCKS_16; subs = PNA2DP_SBC_SUBBANDS_8; }
        if (!blocks || !subs || !alloc || Caps->SbcMinBitpool < 2 || Caps->SbcMinBitpool > Caps->SbcMaxBitpool || Caps->SbcMaxBitpool > 250) return FALSE;
        if (Pref->Sbc.BlockMask && (blocks & Pref->Sbc.BlockMask)) blocks &= Pref->Sbc.BlockMask;
        if (Pref->Sbc.SubbandMask && (subs & Pref->Sbc.SubbandMask)) subs &= Pref->Sbc.SubbandMask;
        if (Pref->Sbc.AllocMask && (alloc & Pref->Sbc.AllocMask)) alloc &= Pref->Sbc.AllocMask;
        Cfg->SbcBlocks = (blocks & PNA2DP_SBC_BLOCKS_16) ? 16 : (blocks & PNA2DP_SBC_BLOCKS_12) ? 12 : (blocks & PNA2DP_SBC_BLOCKS_8) ? 8 : 4;
        Cfg->SbcSubbands = (subs & PNA2DP_SBC_SUBBANDS_8) ? 8 : 4;
        Cfg->SbcAlloc = (alloc & PNA2DP_SBC_ALLOC_LOUDNESS) ? 0 : 1;
        UCHAR minbp = PN_MAX(Caps->SbcMinBitpool, (UCHAR)2);
        UCHAR maxbp = Caps->SbcMaxBitpool;
        if (Pref->Sbc.MaxBitpool) {
            if (Pref->Sbc.MaxBitpool <= maxbp || Pref->Sbc.AllowBitpoolOverride) maxbp = Pref->Sbc.MaxBitpool;
        }
        if (Pref->Sbc.MinBitpool && Pref->Sbc.MinBitpool > minbp) minbp = Pref->Sbc.MinBitpool;
        /* XQ takes the bitpool plain dual channel gets: no fixed 47 cap (known issue, #1/#7) */
        ULONG modeMax = PN_MIN(250u, Cfg->SbcSubbands *
            ((Cfg->ChannelMode == PNA2DP_CHMODE_MONO || Cfg->ChannelMode == PNA2DP_CHMODE_DUAL) ? 16u : 32u));
        maxbp = (UCHAR)PN_MIN(maxbp, modeMax);
        if (maxbp < minbp) return FALSE;
        /* Keep every frame inside a single L2CAP MTU for latency: frame <= ~ (mtu-13) */
        Cfg->SbcMinBitpool = minbp;
        Cfg->SbcMaxBitpool = maxbp;
        Cfg->SbcBitpool = maxbp;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    }
    case PNA2DP_CODEC_AAC:
        if (Pref->Aac.RateMask) rates &= Pref->Aac.RateMask;
        if (!rates) rates = Caps->RateMask;
        if (Pref->Aac.ChannelModeMask) chmodes &= Pref->Aac.ChannelModeMask;
        if (!chmodes) chmodes = Caps->ChannelModeMask;
        Cfg->MediaCodecType = A2DP_CODEC_MPEG24_AAC;
        Cfg->SampleRate = PickRate(rates, PreferredRate);
        Cfg->ChannelMode = (chmodes & PNA2DP_CHMODE_STEREO) ? PNA2DP_CHMODE_STEREO : PNA2DP_CHMODE_MONO;
        Cfg->AacObjectType = 2;
        Cfg->AacObjectMask = (Caps->AacObjectMask & AAC_OBJ_MPEG4_LC) ? AAC_OBJ_MPEG4_LC :
                             (Caps->AacObjectMask & AAC_OBJ_MPEG2_LC) ? AAC_OBJ_MPEG2_LC : 0;
        // The local SEP and encoder currently support LC only.
        if (!Cfg->AacObjectMask) return FALSE;
        Cfg->AacVbr = Caps->AacVbr && Pref->Aac.Vbr;
        Cfg->BitrateBps = Caps->AacMaxBitrate ? Caps->AacMaxBitrate :
            (Pref->Aac.BitrateBps ? Pref->Aac.BitrateBps : 256000);
        if (Pref->Aac.BitrateBps && Pref->Aac.BitrateBps < Cfg->BitrateBps) Cfg->BitrateBps = Pref->Aac.BitrateBps;
        if (Cfg->BitrateBps > 320000) Cfg->BitrateBps = 320000;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_HD:
    case PNA2DP_CODEC_APTX_LL: {
        const PNA2DP_APTX_PARAMS *ap = Caps->Codec == PNA2DP_CODEC_APTX ? &Pref->Aptx : Caps->Codec == PNA2DP_CODEC_APTX_HD ? &Pref->AptxHd : &Pref->AptxLl;
        if (ap->RateMask) rates &= ap->RateMask;
        if (!rates) rates = Caps->RateMask;
        if (!(chmodes & PNA2DP_CHMODE_STEREO)) return FALSE; // bundled encoder requires stereo
        Cfg->MediaCodecType = A2DP_CODEC_VENDOR;
        Cfg->SampleRate = PickRate(rates, PreferredRate);
        Cfg->ChannelMode = (chmodes & PNA2DP_CHMODE_STEREO) ? PNA2DP_CHMODE_STEREO : PNA2DP_CHMODE_MONO;
        Cfg->PcmFormat = PNA2DP_FMT_S24;
        Cfg->AptxLlTargetLevel = ap->LlTargetLevel;
        break;
    }
    case PNA2DP_CODEC_LDAC:
        if (Pref->Ldac.RateMask) rates &= Pref->Ldac.RateMask;
        if (!rates) rates = Caps->RateMask;
        if (Pref->Ldac.ChannelModeMask) chmodes &= Pref->Ldac.ChannelModeMask;
        if (!chmodes) chmodes = Caps->ChannelModeMask;
        Cfg->MediaCodecType = A2DP_CODEC_VENDOR;
        /* LDAC: prefer the engine rate when the sink has it, otherwise the highest */
        Cfg->SampleRate = PickRate(rates, PreferredRate);
        Cfg->ChannelMode = (chmodes & PNA2DP_CHMODE_STEREO) ? PNA2DP_CHMODE_STEREO : (chmodes & PNA2DP_CHMODE_DUAL) ? PNA2DP_CHMODE_DUAL : PNA2DP_CHMODE_MONO;
        if (preferDual && (chmodes & PNA2DP_CHMODE_DUAL)) Cfg->ChannelMode = PNA2DP_CHMODE_DUAL;
        Cfg->LdacEqmid = (Pref->Ldac.Eqmid <= PNA2DP_LDAC_EQMID_MQ) ? Pref->Ldac.Eqmid : PNA2DP_LDAC_EQMID_SQ;
        Cfg->PcmFormat = (Pref->Ldac.FormatMask & PNA2DP_FMT_S32) ? PNA2DP_FMT_S32 : (Pref->Ldac.FormatMask & PNA2DP_FMT_S24) ? PNA2DP_FMT_S24 :
                         (Pref->Ldac.FormatMask & PNA2DP_FMT_F32) ? PNA2DP_FMT_F32 : (PreferredBits > 16 ? PNA2DP_FMT_S32 : PNA2DP_FMT_S16);
        break;
    case PNA2DP_CODEC_FASTSTREAM:
        Cfg->MediaCodecType = A2DP_CODEC_VENDOR;
        Cfg->SampleRate = (rates & PNA2DP_RATE_48000) ? 48000 : 44100;
        Cfg->ChannelMode = PNA2DP_CHMODE_JOINT;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    case PNA2DP_CODEC_OPUS:
        Cfg->MediaCodecType = A2DP_CODEC_VENDOR;
        Cfg->SampleRate = 48000;
        Cfg->ChannelMode = (chmodes & PNA2DP_CHMODE_STEREO) ? PNA2DP_CHMODE_STEREO : PNA2DP_CHMODE_MONO;
        Cfg->PcmFormat = PNA2DP_FMT_S16;
        break;
    default:
        return FALSE;
    }
    if (!Cfg->SampleRate || !Cfg->ChannelMode) return FALSE;
    Cfg->Channels = (Cfg->ChannelMode == PNA2DP_CHMODE_MONO) ? 1 : 2;
    A2dpSerializeConfig(Cfg);
    Cfg->BitrateBps = A2dpNominalBitrate(Cfg);
    Cfg->MaxBitrateBps = Cfg->BitrateBps;
    return TRUE;
}

/* A receiver-chosen configuration (its SET_CONFIGURATION / RECONFIGURE) fixes the rate, channel
 * mode and, for SBC, the bitpool range; the rest is the source's own choice and follows the same
 * preference rules as A2dpSelectConfig. Without this a receiver that connects by itself got the
 * parser's defaults: LDAC SQ (660 kbps) under an HQ preference. */
VOID A2dpApplySourcePreferences(_Inout_ PA2DP_CODEC_CONFIG Cfg, _In_ const PNA2DP_CONFIG *Pref, _In_ BOOLEAN Conservative)
{
    switch (Cfg->Codec) {
    case PNA2DP_CODEC_SBC: {
        /* the receiver's range (lo..hi) is binding. A preference narrows where the encoder starts
         * (SbcBitpool, the ABR's top rung) and its floor; SbcMaxBitpool stays the negotiated maximum,
         * which a live bitpool change may still reach in band (A2dpSbcPlanStream, DeviceSetConfig) */
        const UCHAR lo = Cfg->SbcMinBitpool, hi = Cfg->SbcMaxBitpool;
        const UCHAR top = (Pref->Sbc.MaxBitpool && Pref->Sbc.MaxBitpool < hi) ? (UCHAR)PN_MAX(Pref->Sbc.MaxBitpool, lo) : hi;
        Cfg->SbcMinBitpool = (UCHAR)PN_MIN(PN_MAX(Pref->Sbc.MinBitpool, lo), top);
        Cfg->SbcBitpool = top;
        break;
    }
    case PNA2DP_CODEC_AAC:
        Cfg->AacVbr = Cfg->AacVbr && Pref->Aac.Vbr;
        if (!Cfg->BitrateBps) Cfg->BitrateBps = Pref->Aac.BitrateBps ? Pref->Aac.BitrateBps : 256000;
        if (Pref->Aac.BitrateBps && Pref->Aac.BitrateBps < Cfg->BitrateBps) Cfg->BitrateBps = Pref->Aac.BitrateBps;
        if (Cfg->BitrateBps > 320000) Cfg->BitrateBps = 320000;
        break;
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_HD:
    case PNA2DP_CODEC_APTX_LL:
        Cfg->AptxLlTargetLevel = (Cfg->Codec == PNA2DP_CODEC_APTX ? &Pref->Aptx : Cfg->Codec == PNA2DP_CODEC_APTX_HD ? &Pref->AptxHd : &Pref->AptxLl)->LlTargetLevel;
        break;
    case PNA2DP_CODEC_LDAC:
        Cfg->LdacEqmid = (Pref->Ldac.Eqmid <= PNA2DP_LDAC_EQMID_MQ) ? Pref->Ldac.Eqmid : PNA2DP_LDAC_EQMID_SQ;
        if (Pref->Ldac.FormatMask & PNA2DP_FMT_S32) Cfg->PcmFormat = PNA2DP_FMT_S32;
        else if (Pref->Ldac.FormatMask & PNA2DP_FMT_S24) Cfg->PcmFormat = PNA2DP_FMT_S24;
        else if (Pref->Ldac.FormatMask & PNA2DP_FMT_F32) Cfg->PcmFormat = PNA2DP_FMT_F32;
        break;
    default:
        break;
    }
    Cfg->BitrateBps = Cfg->MaxBitrateBps = A2dpNominalBitrate(Cfg);
    /* the basic-rate budget, as the chooser applies it (a fixed-rate codec above it stays as the
     * receiver configured it). The receiver's IEs and SBC maximum stay what it configured. */
    if (Conservative) {
        UCHAR ies[sizeof(Cfg->Ies)];
        const UCHAR len = Cfg->IesLength, sbcMax = Cfg->SbcMaxBitpool;
        RtlCopyMemory(ies, Cfg->Ies, sizeof(ies));
        (void)A2dpConstrainBasicRadio(Cfg);
        RtlCopyMemory(Cfg->Ies, ies, sizeof(ies));
        Cfg->IesLength = len;
        Cfg->SbcMaxBitpool = sbcMax;
    }
    /* Except AAC's VBR bit and bitrate: the codec host takes the encoder mode and peak from the
     * IEs (aac_plugin), so they carry the source's choice. CBR is allowed where the receiver
     * permits VBR, and the bitrate only goes down. */
    if (Cfg->Codec == PNA2DP_CODEC_AAC && Cfg->IesLength >= 6) {
        Cfg->Ies[3] = (UCHAR)((Cfg->AacVbr ? 0x80 : 0) | ((Cfg->BitrateBps >> 16) & 0x7F));
        Cfg->Ies[4] = (UCHAR)(Cfg->BitrateBps >> 8);
        Cfg->Ies[5] = (UCHAR)Cfg->BitrateBps;
    }
}

/* Conservative initial envelope for a confirmed Basic Rate-only adapter.
 * EDR/unknown radios retain the user's ceiling and are measured during playback.
 * This is a policy budget, not a measured link speed. Never violate sink minima. */
BOOLEAN A2dpConstrainBasicRadio(_Inout_ PA2DP_CODEC_CONFIG Cfg)
{
    const ULONG budget = 350000;
    if (Cfg->Codec == PNA2DP_CODEC_SBC) {
        while (Cfg->SbcBitpool > Cfg->SbcMinBitpool && A2dpNominalBitrate(Cfg) > budget) Cfg->SbcBitpool--;
        Cfg->SbcMaxBitpool = Cfg->SbcBitpool;
    } else if (Cfg->Codec == PNA2DP_CODEC_LDAC) Cfg->LdacEqmid = PNA2DP_LDAC_EQMID_MQ;
    else if (Cfg->Codec == PNA2DP_CODEC_AAC) Cfg->BitrateBps = PN_MIN(Cfg->BitrateBps, budget);
    else if (A2dpNominalBitrate(Cfg) > budget) return FALSE;
    Cfg->MaxBitrateBps = Cfg->BitrateBps = A2dpNominalBitrate(Cfg);
    A2dpSerializeConfig(Cfg);
    return TRUE;
}

VOID A2dpSerializeConfig(_Inout_ PA2DP_CODEC_CONFIG Cfg)
{
    PUCHAR o = Cfg->Ies;
    RtlZeroMemory(Cfg->Ies, sizeof(Cfg->Ies));
    ULONG rateMask = A2dpHzToRateMask(Cfg->SampleRate);
    switch (Cfg->Codec) {
    case PNA2DP_CODEC_SBC:
        o[0] = (UCHAR)(SbcRateMaskToBits(rateMask) | SbcChMaskToBits(Cfg->ChannelMode));
        o[1] = (UCHAR)((Cfg->SbcBlocks == 16 ? PNA2DP_SBC_BLOCKS_16 : Cfg->SbcBlocks == 12 ? PNA2DP_SBC_BLOCKS_12 : Cfg->SbcBlocks == 8 ? PNA2DP_SBC_BLOCKS_8 : PNA2DP_SBC_BLOCKS_4) |
                       (Cfg->SbcSubbands == 8 ? PNA2DP_SBC_SUBBANDS_8 : PNA2DP_SBC_SUBBANDS_4) |
                       (Cfg->SbcAlloc == 0 ? PNA2DP_SBC_ALLOC_LOUDNESS : PNA2DP_SBC_ALLOC_SNR));
        o[2] = Cfg->SbcMinBitpool;
        o[3] = Cfg->SbcMaxBitpool;
        Cfg->IesLength = 4;
        break;
    case PNA2DP_CODEC_AAC: {
        UCHAR b1, b2;
        o[0] = Cfg->AacObjectMask ? Cfg->AacObjectMask :
               (UCHAR)(Cfg->AacObjectType == 5 ? AAC_OBJ_HE_V1 : Cfg->AacObjectType == 29 ? AAC_OBJ_HE_V2 : AAC_OBJ_MPEG4_LC);
        AacRateMaskToBits(rateMask, &b1, &b2);
        o[1] = b1;
        o[2] = (UCHAR)(b2 | (Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? AAC_CH_MONO : AAC_CH_STEREO));
        o[3] = (UCHAR)((Cfg->AacVbr ? 0x80 : 0) | ((Cfg->BitrateBps >> 16) & 0x7F));
        o[4] = (UCHAR)(Cfg->BitrateBps >> 8);
        o[5] = (UCHAR)(Cfg->BitrateBps);
        Cfg->IesLength = 6;
        break;
    }
    case PNA2DP_CODEC_APTX:
        WriteLe32(o, A2DP_VENDOR_APTX); WriteLe16(o + 4, A2DP_CODECID_APTX);
        o[6] = (UCHAR)(AptxRateMaskToBits(rateMask) | (Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? APTX_CH_MONO : APTX_CH_STEREO));
        Cfg->IesLength = 7;
        break;
    case PNA2DP_CODEC_APTX_HD:
        WriteLe32(o, A2DP_VENDOR_APTX_HD); WriteLe16(o + 4, A2DP_CODECID_APTX_HD);
        o[6] = (UCHAR)(AptxRateMaskToBits(rateMask) | (Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? APTX_CH_MONO : APTX_CH_STEREO));
        Cfg->IesLength = 11;
        break;
    case PNA2DP_CODEC_APTX_LL:
        WriteLe32(o, A2DP_VENDOR_CSR); WriteLe16(o + 4, A2DP_CODECID_APTX_LL);
        o[6] = (UCHAR)(AptxRateMaskToBits(rateMask) | (Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? APTX_CH_MONO : APTX_CH_STEREO));
        o[7] = 0;
        Cfg->IesLength = 8;
        break;
    case PNA2DP_CODEC_LDAC:
        WriteLe32(o, A2DP_VENDOR_SONY); WriteLe16(o + 4, A2DP_CODECID_LDAC);
        o[6] = LdacRateMaskToBits(rateMask);
        o[7] = (UCHAR)(Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? LDAC_CH_MONO : Cfg->ChannelMode == PNA2DP_CHMODE_DUAL ? LDAC_CH_DUAL : LDAC_CH_STEREO);
        Cfg->IesLength = 8;
        break;
    case PNA2DP_CODEC_FASTSTREAM:
        WriteLe32(o, A2DP_VENDOR_CSR); WriteLe16(o + 4, A2DP_CODECID_FASTSTREAM);
        o[6] = FASTSTREAM_DIR_SINK;
        o[7] = (UCHAR)(Cfg->SampleRate == 48000 ? FASTSTREAM_SINK_FREQ_48000 : FASTSTREAM_SINK_FREQ_44100);
        Cfg->IesLength = 8;
        break;
    case PNA2DP_CODEC_OPUS:
        WriteLe32(o, A2DP_VENDOR_LINUXFOUND); WriteLe16(o + 4, A2DP_CODECID_OPUS);
        o[6] = (UCHAR)(Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? 0x01 : 0x02);
        Cfg->IesLength = 7;
        break;
    default:
        Cfg->IesLength = 0;
        break;
    }
}

ULONG A2dpNominalBitrate(_In_ const A2DP_CODEC_CONFIG *Cfg)
{
    switch (Cfg->Codec) {
    case PNA2DP_CODEC_SBC: {
        sbc_params p;
        p.sample_rate = Cfg->SampleRate;
        p.channel_mode = (sbc_u8)(Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : Cfg->ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                                  Cfg->ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
        p.allocation = Cfg->SbcAlloc;
        p.subbands = Cfg->SbcSubbands;
        p.blocks = Cfg->SbcBlocks;
        p.bitpool = Cfg->SbcBitpool ? Cfg->SbcBitpool : Cfg->SbcMaxBitpool;
        return sbc_bitrate(&p);
    }
    case PNA2DP_CODEC_AAC:
        return Cfg->BitrateBps;
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
        return Cfg->SampleRate * 4 * Cfg->Channels;             /* 4:1 of 16-bit: 352.8 kbps at 44.1 stereo */
    case PNA2DP_CODEC_APTX_HD:
        return Cfg->SampleRate * 6 * Cfg->Channels;             /* 24-bit 4:1: 576 kbps at 48 stereo */
    case PNA2DP_CODEC_LDAC: {
        BOOLEAN x = (Cfg->SampleRate == 44100 || Cfg->SampleRate == 88200);
        switch (Cfg->LdacEqmid) {
        case PNA2DP_LDAC_EQMID_HQ: return x ? 909000 : 990000;
        case PNA2DP_LDAC_EQMID_SQ: return x ? 606000 : 660000;
        default: return x ? 303000 : 330000;
        }
    }
    case PNA2DP_CODEC_FASTSTREAM:
        return 212000;
    case PNA2DP_CODEC_OPUS:
        return Cfg->BitrateBps ? Cfg->BitrateBps : 160000;
    default:
        return 0;
    }
}

ULONG A2dpQualityAtLevel(_In_ const A2DP_CODEC_CONFIG *Base, ULONG Level, ULONG FloorBps, _Out_opt_ A2DP_CODEC_CONFIG *Result)
{
    A2DP_CODEC_CONFIG cfg = *Base;
    switch (cfg.Codec) {
    case PNA2DP_CODEC_SBC: {
        /* ladder: max bitpool down to min in steps of ~15%, plus the "fits 2-DH5" rung */
        ULONG maxbp = cfg.SbcMaxBitpool, minbp = PN_MAX(cfg.SbcMinBitpool, 2u);
        ULONG bp = PnSbcLadderBitpool(maxbp, minbp, cfg.SbcFitBitpool, Level);
        if (bp < minbp) bp = minbp;
        if (bp > maxbp) bp = maxbp;
        cfg.SbcBitpool = (UCHAR)bp;
        while (cfg.SbcBitpool < maxbp && A2dpNominalBitrate(&cfg) < FloorBps) cfg.SbcBitpool++;
        break;
    }
    case PNA2DP_CODEC_LDAC:
        cfg.LdacEqmid = (UCHAR)PnLdacAbrQuality(Base->LdacEqmid, Level);
        while (cfg.LdacEqmid > Base->LdacEqmid && A2dpNominalBitrate(&cfg) < FloorBps) cfg.LdacEqmid--;
        break;
    case PNA2DP_CODEC_AAC: {
        /* rung n is the n-th step below the ceiling: a ceiling under 256 kbps (the
         * receiver's, or the MTU fit) does not repeat the top as rung 1 */
        static const ULONG ladder[] = { 256000, 192000, 128000, 96000 };
        ULONG b = cfg.MaxBitrateBps;
        for (ULONG i = 0, n = 0; n < Level && i < PN_ARRAYSIZE(ladder); i++) {
            if (ladder[i] >= cfg.MaxBitrateBps) continue;
            b = ladder[i];
            n++;
        }
        cfg.BitrateBps = PnQualityBoundBitrate(b, FloorBps, cfg.MaxBitrateBps);
        break;
    }
    default:
        break;
    }
    cfg.BitrateBps = A2dpNominalBitrate(&cfg);
    if (Result) *Result = cfg;
    return cfg.BitrateBps;
}

static VOID SbcParamsFor(_In_ const A2DP_CODEC_CONFIG *Cfg, ULONG Bitpool, _Out_ sbc_params *P)
{
    P->sample_rate = Cfg->SampleRate;
    P->channel_mode = (sbc_u8)(Cfg->ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : Cfg->ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                               Cfg->ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
    P->allocation = Cfg->SbcAlloc;
    P->subbands = Cfg->SbcSubbands;
    P->blocks = Cfg->SbcBlocks;
    P->bitpool = (sbc_u8)Bitpool;
}

ULONG A2dpSbcFramesPerPacket(_In_ const A2DP_CODEC_CONFIG *Cfg, ULONG TargetLatencyMs, ULONG PayloadRoom, ULONG HeaderBytes, ULONG SduBytes)
{
    if (Cfg->Codec != PNA2DP_CODEC_SBC || !Cfg->SampleRate) return 0;
    sbc_params p;
    SbcParamsFor(Cfg, Cfg->SbcBitpool ? Cfg->SbcBitpool : Cfg->SbcMaxBitpool, &p);
    ULONG len = sbc_frame_length(&p);
    if (!len || len > PayloadRoom) return 0;
    ULONG frameUs = (ULONG)((ULONGLONG)Cfg->SbcSubbands * Cfg->SbcBlocks * 1000000u / Cfg->SampleRate);
    /* the 2-DH5 rung packs for one 2-DH5 packet, which is why it exists */
    ULONG sdu = (Cfg->SbcFitBitpool && Cfg->SbcBitpool == Cfg->SbcFitBitpool) ? PN_SDU_2DH5 : SduBytes;
    return PnSbcPacking(len, frameUs, TargetLatencyMs, PayloadRoom, HeaderBytes, sdu);
}

BOOLEAN A2dpSbcPlanStream(_In_ const A2DP_CODEC_CONFIG *Cfg, _In_ BOOLEAN Xq, _In_ ULONG LocalSdu, _In_ ULONG XqSdu,
                          _In_ ULONG PayloadRoom, _In_ ULONG HeaderBytes, _In_ ULONG TargetLatencyMs, _Out_ A2DP_SBC_STREAM_PLAN *Plan)
{
    RtlZeroMemory(Plan, sizeof(*Plan));
    if (Cfg->Codec != PNA2DP_CODEC_SBC) return FALSE;
    sbc_params p;
    SbcParamsFor(Cfg, Cfg->SbcMaxBitpool, &p);
    /* XQ (dual 8/16): the bitpool of plain dual channel; only the packing follows the EDR packet
     * both ends take (the 2-DH5 point when the sink's features or the radio are unknown) */
    BOOLEAN xq = Xq && p.channel_mode == SBC_MODE_DUAL && p.subbands == 8 && p.blocks == 16;
    ULONG sdu = xq ? (XqSdu ? XqSdu : PN_SDU_2DH5) : LocalSdu;
    /* keep one frame within the MTU */
    ULONG maxbp = sbc_max_bitpool_for(&p, 0, PayloadRoom);
    if (maxbp < Cfg->SbcMinBitpool) return FALSE;
    ULONG ceiling = PN_MIN((ULONG)Cfg->SbcMaxBitpool, maxbp);
    ULONG start = Cfg->SbcBitpool ? PN_MIN((ULONG)Cfg->SbcBitpool, ceiling) : ceiling;
    if (start < Cfg->SbcMinBitpool) start = Cfg->SbcMinBitpool;
    A2DP_CODEC_CONFIG c = *Cfg;
    c.SbcBitpool = (UCHAR)start;
    c.SbcMaxBitpool = (UCHAR)start;   /* a lowered live bitpool stays the ABR's top rung (finding #6) */
    c.SbcFitBitpool = 0;
    ULONG frames = A2dpSbcFramesPerPacket(&c, TargetLatencyMs, PayloadRoom, HeaderBytes, sdu);
    if (!frames) return FALSE;
    /* ABR rung at which these packets fit one 2-DH5 packet, where they might not today */
    if (sdu == 0 || sdu > PN_SDU_2DH5) {
        UCHAR fit = A2dpSbcBitpoolForPacket(&c, frames, HeaderBytes, PN_SDU_2DH5);
        if (fit && fit < start) Plan->FitBitpool = fit;
    }
    Plan->StartBitpool = (UCHAR)start;
    Plan->CeilingBitpool = (UCHAR)ceiling;
    Plan->SduBytes = sdu;
    Plan->FramesPerPacket = frames;
    Plan->Xq = xq;
    return TRUE;
}

UCHAR A2dpSbcBitpoolForPacket(_In_ const A2DP_CODEC_CONFIG *Cfg, ULONG Frames, ULONG HeaderBytes, ULONG SduBytes)
{
    if (Cfg->Codec != PNA2DP_CODEC_SBC || !Frames || SduBytes <= HeaderBytes) return 0;
    A2DP_CODEC_CONFIG q = *Cfg;
    for (ULONG bp = Cfg->SbcMaxBitpool; bp >= PN_MAX((ULONG)Cfg->SbcMinBitpool, 2u) && bp <= 250; bp--) {
        q.SbcBitpool = (UCHAR)bp;
        sbc_params p;
        p.sample_rate = q.SampleRate;
        p.channel_mode = (sbc_u8)(q.ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : q.ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                                  q.ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
        p.allocation = q.SbcAlloc; p.subbands = q.SbcSubbands; p.blocks = q.SbcBlocks; p.bitpool = (sbc_u8)bp;
        ULONG len = sbc_frame_length(&p);
        if (len && HeaderBytes + (ULONGLONG)Frames * len <= SduBytes) return (UCHAR)bp;
    }
    return 0;
}
