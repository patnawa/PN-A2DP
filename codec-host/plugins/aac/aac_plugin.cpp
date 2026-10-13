/*
 * aac_plugin.cpp - AAC-LC plugin using Fraunhofer FDK AAC (fdk-aac).
 *
 * Output DLL: pna2dp-codec-aac.dll. The encoder emits one LATM/LOAS-less (MCP1) frame
 * per 1024 PCM frames, which is exactly the A2DP AAC media payload (RTP header + LATM).
 * Many sinks do not reassemble an access unit split over several RTP packets, so the
 * peak and target bitrates are capped (on open and on every reconfigure) so that one
 * access unit plus LATM always fits the payload room the driver passes as MediaMtu.
 *
 * SPDX-License-Identifier: MIT (plugin); fdk-aac is under the Fraunhofer FDK AAC
 * Codec Library for Android license (see third_party/fdk-aac/NOTICE)
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "audio_policy.h"     /* PnAacMaxBitrateForPayload: same formula as the driver */
#include "aacenc_lib.h"
#include <stdlib.h>
#include <string.h>

struct PnCodecEncoder {
    HANDLE_AACENCODER enc;
    uint32_t channels;
    uint32_t rate;
    uint32_t bitrate;
    uint32_t mtu;
    uint32_t fit;      /* largest bitrate whose access unit fits one packet, 0 = unknown */
    uint32_t declared; /* bitrate configured with the sink (CodecInfo[3..5]), 0 = none */
    uint32_t level;    /* driver ABR level (QualityIndex): 0 = the configured quality */
    int vbr;
};

/* CBR audio bandwidth: FDK's table cuts every CBR rate from 96 kbps per channel at 17 kHz,
 * so 256-320 kbps spent its bits on a 17 kHz signal (finding #11). 0 = FDK's table. */
static uint32_t CbrBandwidth(uint32_t perChannel)
{
    return perChannel >= 128000 ? 20000u : perChannel >= 96000 ? 19000u : 0u;
}

static int SetBitrate(PnCodecEncoder *e, uint32_t bitrate)
{
    if (bitrate == 0) bitrate = 256000;
    if (bitrate > PN_AAC_MAX_BITRATE) bitrate = PN_AAC_MAX_BITRATE;
    if (e->fit && bitrate > e->fit) bitrate = e->fit;
    /* FDK bitrate modes: 0 CBR, 1..5 VBR (very low .. very high). VBR ignores AACENC_BITRATE:
     * its mode follows the content, so the first ABR step (320 -> 256 kbps, both mode 5)
     * changed nothing and lower rungs overshot. VBR is the configured quality only; every
     * reduction is CBR at exactly the rung's bitrate (finding #10). */
    int vbr = e->vbr && e->level == 0;
    /* The peak bounds every access unit (FDK uses max(bitrate, peak)): within the packet fit,
     * and in VBR within the bitrate the sink was configured with (finding #12: VBR averaged
     * 12% over a declared 256 kbps; CBR averages its bitrate). It stays there after an ABR
     * reduction so the bit reservoir still works. */
    uint32_t peak = e->fit ? e->fit : PN_AAC_MAX_BITRATE;
    if (vbr && e->declared && e->declared < peak) peak = e->declared;
    if (aacEncoder_SetParam(e->enc, AACENC_PEAK_BITRATE, peak) != AACENC_OK) return -1;
    uint32_t perChannel = bitrate / e->channels;
    if (vbr) {
        int mode = perChannel >= 128000 ? 5 : perChannel >= 96000 ? 4 : perChannel >= 64000 ? 3 : perChannel >= 48000 ? 2 : 1;
        if (aacEncoder_SetParam(e->enc, AACENC_BITRATEMODE, (UINT)mode) != AACENC_OK) return -1;
        if (aacEncoder_SetParam(e->enc, AACENC_BANDWIDTH, 0) != AACENC_OK) return -1;
    } else {
        if (aacEncoder_SetParam(e->enc, AACENC_BITRATEMODE, 0) != AACENC_OK) return -1;
        if (aacEncoder_SetParam(e->enc, AACENC_BANDWIDTH, CbrBandwidth(perChannel)) != AACENC_OK) return -1;
    }
    if (aacEncoder_SetParam(e->enc, AACENC_BITRATE, bitrate) != AACENC_OK) return -1;
    e->bitrate = bitrate;
    return 0;
}

static PnCodecEncoder *AacOpen(const PNA2DP_STREAM_PARAMS *p)
{
    if (p->Codec != PNA2DP_CODEC_AAC || p->PcmFormat != PNA2DP_FMT_S16 ||
        (p->Channels != 1 && p->Channels != 2) || p->CodecInfoLength != 6 ||
        (p->CodecInfo[0] != 0x80 && p->CodecInfo[0] != 0x40)) return nullptr;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return nullptr;
    e->channels = p->Channels;
    e->rate = p->SampleRate;
    e->mtu = p->MediaMtu;
    e->fit = PnAacMaxBitrateForPayload(p->MediaMtu, p->SampleRate);
    e->vbr = (p->CodecInfoLength >= 4) ? ((p->CodecInfo[3] & 0x80) != 0) : 0;
    e->declared = ((uint32_t)(p->CodecInfo[3] & 0x7f) << 16) | ((uint32_t)p->CodecInfo[4] << 8) | p->CodecInfo[5];
    e->level = p->QualityIndex;
    if (aacEncOpen(&e->enc, 0x01, e->channels) != AACENC_OK) { free(e); return nullptr; }
    if (aacEncoder_SetParam(e->enc, AACENC_AOT, AOT_AAC_LC) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_SAMPLERATE, e->rate) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_CHANNELMODE, e->channels == 1 ? MODE_1 : MODE_2) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_CHANNELORDER, 1) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_TRANSMUX, TT_MP4_LATM_MCP1) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_HEADER_PERIOD, 1) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_AFTERBURNER, 1) != AACENC_OK ||
        aacEncoder_SetParam(e->enc, AACENC_GRANULE_LENGTH, 1024) != AACENC_OK ||
        SetBitrate(e, p->BitrateBps) != 0 || aacEncEncode(e->enc, NULL, NULL, NULL, NULL) != AACENC_OK) {
        aacEncClose(&e->enc); free(e); return nullptr;
    }
    return e;
}

static int AacEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    if (pcm_frames != 1024 || pcm_bytes != pcm_frames * e->channels * sizeof(INT16)) return -1;
    AACENC_BufDesc in = {}, outd = {};
    AACENC_InArgs inArgs = {};
    AACENC_OutArgs outArgs = {};
    void *inPtr = (void *)pcm;
    INT inId = IN_AUDIO_DATA, inSize = (INT)pcm_bytes, inEl = sizeof(INT16);
    in.numBufs = 1; in.bufs = &inPtr; in.bufferIdentifiers = &inId; in.bufSizes = &inSize; in.bufElSizes = &inEl;
    void *outPtr = out->buffer;
    INT outId = OUT_BITSTREAM_DATA, outSize = (INT)out->capacity, outEl = 1;
    outd.numBufs = 1; outd.bufs = &outPtr; outd.bufferIdentifiers = &outId; outd.bufSizes = &outSize; outd.bufElSizes = &outEl;
    inArgs.numInSamples = (INT)(pcm_bytes / sizeof(INT16));
    AACENC_ERROR err = aacEncEncode(e->enc, &in, &outd, &inArgs, &outArgs);
    if (err != AACENC_OK) return -1;
    if ((uint32_t)outArgs.numInSamples != pcm_frames * e->channels) return -1;
    out->written = (uint32_t)outArgs.numOutBytes;
    out->frame_count = outArgs.numOutBytes ? 1 : 0;
    out->pcm_frames_consumed = outArgs.numOutBytes ? 1024 : 0; // samples represented by this RTP payload
    out->payload_header_byte = 0;
    out->actual_bitrate_bps = e->bitrate;
    return 0;
}

/* quality: the driver's ABR level (0 = the configured quality); bitrate: the rung's. */
static int AacReconfigure(PnCodecEncoder *e, uint32_t quality, uint32_t bitrate)
{
    if (quality == e->level && (!bitrate || bitrate == e->bitrate)) return 0;
    uint32_t oldLevel = e->level;
    e->level = quality;
    if (SetBitrate(e, bitrate ? bitrate : e->bitrate) != 0) { e->level = oldLevel; return -1; }   /* re-applies the caps */
    return 0;
}

static void AacFlush(PnCodecEncoder *) {}
static void AacClose(PnCodecEncoder *e) { aacEncClose(&e->enc); free(e); }
static uint32_t AacMaxOut(const PNA2DP_STREAM_PARAMS *p) { return p->MediaMtu + 64 > 1536 ? p->MediaMtu + 64 : 1536; }

static const PnCodecPlugin g_Plugin = {
    PNA2DP_PLUGIN_API_VERSION, "fdk-aac", "2.0.3", "FDK-AAC", PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC),
    AacOpen, AacEncode, AacReconfigure, AacFlush, AacClose, AacMaxOut
};

extern "C" __declspec(dllexport) const PnCodecPlugin *PnCodecPluginGetInfo(void) { return &g_Plugin; }
