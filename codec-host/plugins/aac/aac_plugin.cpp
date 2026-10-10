/*
 * aac_plugin.cpp - AAC-LC / HE-AAC plugin using Fraunhofer FDK AAC (fdk-aac).
 *
 * Output DLL: pna2dp-codec-aac.dll. The encoder emits one LATM/LOAS-less (MCP1) frame
 * per 1024 PCM frames, which is exactly the A2DP AAC media payload (RTP header + LATM).
 *
 * SPDX-License-Identifier: MIT (plugin); fdk-aac is under the Fraunhofer FDK AAC
 * Codec Library for Android license (see third_party/fdk-aac/NOTICE)
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "aacenc_lib.h"
#include <stdlib.h>
#include <string.h>

struct PnCodecEncoder {
    HANDLE_AACENCODER enc;
    uint32_t channels;
    uint32_t rate;
    uint32_t bitrate;
    uint32_t mtu;
    int vbr;
};

static int SetBitrate(PnCodecEncoder *e, uint32_t bitrate)
{
    if (bitrate == 0) bitrate = 256000;
    if (bitrate > 320000) bitrate = 320000;
    uint32_t perChannel = bitrate / e->channels;
    /* FDK bitrate modes: 0 CBR, 1..5 VBR (very low .. very high); keep within the sink's peak */
    if (e->vbr) {
        int mode = perChannel >= 128000 ? 5 : perChannel >= 96000 ? 4 : perChannel >= 64000 ? 3 : perChannel >= 48000 ? 2 : 1;
        aacEncoder_SetParam(e->enc, AACENC_BITRATEMODE, (UINT)mode);
    } else {
        aacEncoder_SetParam(e->enc, AACENC_BITRATEMODE, 0);
    }
    e->bitrate = bitrate;
    return aacEncoder_SetParam(e->enc, AACENC_BITRATE, bitrate) == AACENC_OK ? 0 : -1;
}

static PnCodecEncoder *AacOpen(const PNA2DP_STREAM_PARAMS *p)
{
    if (p->Codec != PNA2DP_CODEC_AAC || p->PcmFormat != PNA2DP_FMT_S16) return nullptr;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return nullptr;
    e->channels = p->Channels;
    e->rate = p->SampleRate;
    e->mtu = p->MediaMtu;
    e->vbr = (p->CodecInfoLength >= 4) ? ((p->CodecInfo[3] & 0x80) != 0) : 0;
    if (aacEncOpen(&e->enc, 0x01, e->channels) != AACENC_OK) { free(e); return nullptr; }
    UINT aot = AOT_AAC_LC;
    if (p->CodecInfoLength >= 1 && (p->CodecInfo[0] & 0x08) && !(p->CodecInfo[0] & 0xC0)) aot = AOT_SBR;   /* HE-AAC v1 only if LC is not offered */
    aacEncoder_SetParam(e->enc, AACENC_AOT, aot);
    aacEncoder_SetParam(e->enc, AACENC_SAMPLERATE, e->rate);
    aacEncoder_SetParam(e->enc, AACENC_CHANNELMODE, e->channels == 1 ? MODE_1 : MODE_2);
    aacEncoder_SetParam(e->enc, AACENC_CHANNELORDER, 1);   /* WAV order */
    aacEncoder_SetParam(e->enc, AACENC_TRANSMUX, TT_MP4_LATM_MCP1);
    aacEncoder_SetParam(e->enc, AACENC_HEADER_PERIOD, 1);
    aacEncoder_SetParam(e->enc, AACENC_AFTERBURNER, 1);
    aacEncoder_SetParam(e->enc, AACENC_PEAK_BITRATE, p->BitrateBps ? p->BitrateBps : 320000);
    aacEncoder_SetParam(e->enc, AACENC_GRANULE_LENGTH, 1024);
    if (SetBitrate(e, p->BitrateBps) != 0 || aacEncEncode(e->enc, NULL, NULL, NULL, NULL) != AACENC_OK) {
        aacEncClose(&e->enc); free(e); return nullptr;
    }
    return e;
}

static int AacEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    (void)pcm_frames;
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
    out->written = (uint32_t)outArgs.numOutBytes;
    out->frame_count = outArgs.numOutBytes ? 1 : 0;
    out->pcm_frames_consumed = (uint32_t)outArgs.numInSamples / e->channels;
    out->payload_header_byte = 0;
    out->actual_bitrate_bps = e->bitrate;
    return 0;
}

static int AacReconfigure(PnCodecEncoder *e, uint32_t quality, uint32_t bitrate)
{
    (void)quality;
    if (bitrate && bitrate != e->bitrate) return SetBitrate(e, bitrate);
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
