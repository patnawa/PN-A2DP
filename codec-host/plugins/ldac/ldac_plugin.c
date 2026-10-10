/*
 * ldac_plugin.c - LDAC encoder plugin using Sony's libldac (Apache-2.0, AOSP).
 *
 * Build: see codec-host/plugins/ldac/build.ps1. Links statically against
 * third_party/ldacBT (libldac + libldacBT_abr). Output DLL: pna2dp-codec-ldac.dll.
 *
 * LDAC specifics:
 *   - encoder consumes 128 (44.1/48 k) or 256 (88.2/96 k) PCM frames per LDAC frame
 *   - EQMID 0/1/2 = 990/660/330 kbps class; may be changed between frames (in band)
 *   - the A2DP packet carries RTP header + 1-byte frame count + N LDAC frames; the
 *     library packs as many frames as fit the MTU for us (ldacBT_encode)
 *   - ABR: libldacBT_abr adjusts EQMID from the transport queue depth we report
 *
 * SPDX-License-Identifier: MIT (plugin); libldac is Apache-2.0
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "ldacBT.h"
#include "ldacBT_abr.h"
#include <stdlib.h>
#include <string.h>

struct PnCodecEncoder {
    HANDLE_LDAC_BT     h;
    HANDLE_LDAC_ABR    abr;
    int                eqmid;
    int                abr_enabled;
    LDACBT_SMPL_FMT_T  fmt;
    uint32_t           rate;
    uint32_t           channels;
    uint32_t           pcm_per_frame;
    uint32_t           mtu;
    uint32_t           bytes_per_sample;
};

static int LdacChannelMode(uint32_t mode)
{
    switch (mode) {
    case PNA2DP_CHMODE_MONO: return LDACBT_CHANNEL_MODE_MONO;
    case PNA2DP_CHMODE_DUAL: return LDACBT_CHANNEL_MODE_DUAL_CHANNEL;
    default: return LDACBT_CHANNEL_MODE_STEREO;
    }
}

static PnCodecEncoder *LdacOpen(const PNA2DP_STREAM_PARAMS *p)
{
    if (p->Codec != PNA2DP_CODEC_LDAC) return NULL;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->h = ldacBT_get_handle();
    if (!e->h) { free(e); return NULL; }
    switch (p->PcmFormat) {
    case PNA2DP_FMT_S16: e->fmt = LDACBT_SMPL_FMT_S16; e->bytes_per_sample = 2; break;
    case PNA2DP_FMT_S24: e->fmt = LDACBT_SMPL_FMT_S24; e->bytes_per_sample = 3; break;
    case PNA2DP_FMT_F32: e->fmt = LDACBT_SMPL_FMT_F32; e->bytes_per_sample = 4; break;
    default:             e->fmt = LDACBT_SMPL_FMT_S32; e->bytes_per_sample = 4; break;
    }
    e->rate = p->SampleRate;
    e->channels = p->Channels;
    e->mtu = p->MediaMtu;
    e->eqmid = (p->QualityIndex <= 2) ? (int)p->QualityIndex : LDACBT_EQMID_SQ;
    e->abr_enabled = (p->QualityIndex == PNA2DP_LDAC_EQMID_ABR);
    if (e->abr_enabled) e->eqmid = LDACBT_EQMID_SQ;
    /* mtu is the payload room: libldac wants the full L2CAP MTU (it subtracts RTP + header itself) */
    int r = ldacBT_init_handle_encode(e->h, (int)(p->MediaMtu + 13), e->eqmid, LdacChannelMode(p->ChannelMode), e->fmt, (int)p->SampleRate);
    if (r != 0) { ldacBT_free_handle(e->h); free(e); return NULL; }
    e->pcm_per_frame = (uint32_t)ldacBT_get_sampling_freq(e->h) >= 88200 ? 256 : 128;
    if (e->abr_enabled) {
        e->abr = ldac_ABR_get_handle();
        if (e->abr) {
            /* interval = number of encode calls between adjustments; 1 Tx = one packet */
            ldac_ABR_Init(e->abr, 1000 /* us per packet is ~ interval hint */);
            ldac_ABR_set_thresholds(e->abr, 6, 4, 2);
        }
    }
    return e;
}

static int LdacEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    (void)pcm_bytes;
    uint32_t frame_bytes = e->pcm_per_frame * e->channels * e->bytes_per_sample;
    uint32_t written = 0, frames_out = 0, consumed = 0;
    for (uint32_t f = 0; f + e->pcm_per_frame <= pcm_frames; f++) {
        int pcm_used = 0, stream_wrote = 0, frame_num = 0;
        if (written + e->mtu > out->capacity) break;
        int r = ldacBT_encode(e->h, (void *)(pcm + (size_t)f * frame_bytes), &pcm_used, out->buffer + written, &stream_wrote, &frame_num);
        if (r != 0) return -1;
        consumed += e->pcm_per_frame;
        if (stream_wrote > 0) {
            written += (uint32_t)stream_wrote;
            frames_out += (uint32_t)frame_num;
            /* libldac emits a whole packet's worth at once; stop so the driver sends it */
            break;
        }
    }
    out->written = written;
    out->frame_count = frames_out;
    out->pcm_frames_consumed = consumed ? consumed : pcm_frames;
    out->payload_header_byte = frames_out & 0x0F;
    out->actual_bitrate_bps = (uint32_t)ldacBT_get_bitrate(e->h) * 1000u;
    return 0;
}

static int LdacReconfigure(PnCodecEncoder *e, uint32_t quality, uint32_t bitrate)
{
    (void)bitrate;
    if (quality == PNA2DP_LDAC_EQMID_ABR) { e->abr_enabled = 1; return 0; }
    e->abr_enabled = 0;
    if (quality <= 2 && (int)quality != e->eqmid) {
        e->eqmid = (int)quality;
        return ldacBT_set_eqmid(e->h, e->eqmid);
    }
    return 0;
}

static void LdacFlush(PnCodecEncoder *e) { (void)e; }

static void LdacClose(PnCodecEncoder *e)
{
    if (e->abr) ldac_ABR_free_handle(e->abr);
    if (e->h) { ldacBT_close_handle(e->h); ldacBT_free_handle(e->h); }
    free(e);
}

static uint32_t LdacMaxOut(const PNA2DP_STREAM_PARAMS *p) { return p->MediaMtu + 64; }

static const PnCodecPlugin g_Plugin = {
    PNA2DP_PLUGIN_API_VERSION, "libldac", "2.0.2.3", "Apache-2.0", PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC),
    LdacOpen, LdacEncode, LdacReconfigure, LdacFlush, LdacClose, LdacMaxOut
};

__declspec(dllexport) const PnCodecPlugin *PnCodecPluginGetInfo(void) { return &g_Plugin; }
