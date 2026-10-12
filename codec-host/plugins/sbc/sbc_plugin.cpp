/*
 * sbc_plugin.cpp - built-in reference plugin: SBC through the shared codec.
 *
 * The kernel already encodes SBC itself; this plugin exists to exercise the host
 * path end to end (set `hostonly=1` with pna2dpctl) and as a template for other
 * plugins.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "sbc/sbc.h"
#include <stdlib.h>
#include <string.h>

struct PnCodecEncoder {
    sbc_encoder enc;
    sbc_params params;
    uint32_t mtu;
};

static PnCodecEncoder *SbcOpen(const PNA2DP_STREAM_PARAMS *p)
{
    if (p->Codec != PNA2DP_CODEC_SBC || p->PcmFormat != PNA2DP_FMT_S16 || p->CodecInfoLength < 4) return nullptr;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return nullptr;
    const uint8_t *ie = p->CodecInfo;
    e->params.sample_rate = p->SampleRate;
    e->params.channel_mode = (uint8_t)(p->ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : p->ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                                       p->ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
    e->params.blocks = (ie[1] & 0x10) ? 16 : (ie[1] & 0x20) ? 12 : (ie[1] & 0x40) ? 8 : 4;
    e->params.subbands = (ie[1] & 0x04) ? 8 : 4;
    e->params.allocation = (sbc_u8)((ie[1] & 0x01) ? SBC_ALLOC_LOUDNESS : SBC_ALLOC_SNR);
    e->params.bitpool = (uint8_t)(p->QualityIndex ? p->QualityIndex : ie[3]);
    e->mtu = p->MediaMtu;
    if (sbc_encoder_init(&e->enc, &e->params) != 0) { free(e); return nullptr; }
    return e;
}

static int SbcEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    uint32_t nch = e->enc.channels;
    uint32_t per = e->enc.pcm_per_frame;
    uint32_t frames = pcm_frames / per;
    uint32_t written = 0, count = 0;
    (void)pcm_bytes;
    for (uint32_t f = 0; f < frames && count < 15; f++) {
        if (written + e->enc.frame_length > out->capacity || written + e->enc.frame_length > e->mtu) break;
        uint32_t n = sbc_encode_frame(&e->enc, (const sbc_s16 *)pcm + (size_t)f * per * nch, out->buffer + written, out->capacity - written);
        if (!n) break;
        written += n;
        count++;
    }
    out->written = written;
    out->frame_count = count;
    out->pcm_frames_consumed = count * per;
    out->payload_header_byte = count & 0x0F;
    out->actual_bitrate_bps = sbc_bitrate(&e->enc.p);
    return 0;
}

static int SbcReconfigure(PnCodecEncoder *e, uint32_t quality, uint32_t bitrate)
{
    (void)bitrate;
    if (quality >= 2 && quality <= 250) sbc_encoder_set_bitpool(&e->enc, (uint8_t)quality);
    return 0;
}

static void SbcFlush(PnCodecEncoder *) {}
static void SbcClose(PnCodecEncoder *e) { free(e); }
static uint32_t SbcMaxOut(const PNA2DP_STREAM_PARAMS *) { return 2048; }

static const PnCodecPlugin g_SbcPlugin = {
    PNA2DP_PLUGIN_API_VERSION, "sbc (built-in)", "0.1", "MIT", PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC),
    SbcOpen, SbcEncode, SbcReconfigure, SbcFlush, SbcClose, SbcMaxOut
};

extern "C" const PnCodecPlugin *PnBuiltinSbcPlugin(void) { return &g_SbcPlugin; }
