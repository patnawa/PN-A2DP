/*
 * ldac_plugin.c - LDAC encoder plugin using Sony's libldac (Apache-2.0, AOSP).
 *
 * Build: codec-host/plugins/ldac/build.ps1 (links libldac + libldacBT_abr statically).
 * Output DLL: pna2dp-codec-ldac.dll.
 *
 * LDAC specifics:
 *   - the encoder consumes 128 (44.1/48 k) or 256 (88.2/96 k) PCM frames per LDAC frame
 *   - EQMID 0/1/2 = 990/660/330 kbps class; it may change between frames (in band)
 *   - libldac packs as many frames as fit the L2CAP MTU and emits whole packets; the
 *     driver sends one packet per result, so packets are queued here when the library
 *     emits more than one for a block, and partial frames are carried over
 *
 * SPDX-License-Identifier: MIT (plugin); libldac is Apache-2.0
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "ldacBT.h"
#include "ldacBT_abr.h"
#include <stdlib.h>
#include <string.h>

#define LDAC_MAX_PACKET      2048
#define LDAC_MAX_PENDING     4
#define LDAC_MAX_LEFTOVER    (128 * 2 * 4)   /* one LSU of 32-bit stereo */

typedef struct ldac_packet {
    uint8_t  data[LDAC_MAX_PACKET];
    uint32_t len;
    uint32_t frames;
} ldac_packet;

struct PnCodecEncoder {
    HANDLE_LDAC_BT     h;
    HANDLE_LDAC_ABR    abr;
    int                eqmid;
    int                abr_enabled;
    LDACBT_SMPL_FMT_T  fmt;
    uint32_t           rate;
    uint32_t           channels;
    uint32_t           pcm_per_frame;
    uint32_t           bytes_per_sample;
    uint32_t           frame_bytes;         /* PCM bytes per encode call (one LSU) */
    uint32_t           mtu;
    /* queued output packets (FIFO) */
    ldac_packet        pending[LDAC_MAX_PENDING];
    uint32_t           pending_head, pending_count;
    /* partial frame carried over between calls */
    uint8_t            leftover[LDAC_MAX_LEFTOVER];
    uint32_t           leftover_bytes;
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
    if (p->Codec != PNA2DP_CODEC_LDAC || (p->Channels != 1 && p->Channels != 2) ||
        (p->ChannelMode != PNA2DP_CHMODE_MONO && p->ChannelMode != PNA2DP_CHMODE_DUAL && p->ChannelMode != PNA2DP_CHMODE_STEREO) ||
        p->Channels != (p->ChannelMode == PNA2DP_CHMODE_MONO ? 1u : 2u) ||
        p->MediaMtu > LDAC_MAX_PACKET || p->MediaMtu < 666) return NULL;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->h = ldacBT_get_handle();
    if (!e->h) { free(e); return NULL; }
    switch (p->PcmFormat) {
    case PNA2DP_FMT_S16: e->fmt = LDACBT_SMPL_FMT_S16; e->bytes_per_sample = 2; break;
    case PNA2DP_FMT_S24: e->fmt = LDACBT_SMPL_FMT_S24; e->bytes_per_sample = 3; break;
    case PNA2DP_FMT_F32: e->fmt = LDACBT_SMPL_FMT_F32; e->bytes_per_sample = 4; break;
    case PNA2DP_FMT_S32: e->fmt = LDACBT_SMPL_FMT_S32; e->bytes_per_sample = 4; break;
    default: ldacBT_free_handle(e->h); free(e); return NULL;
    }
    e->rate = p->SampleRate;
    e->channels = p->Channels ? p->Channels : 2;
    e->mtu = p->MediaMtu;
    e->eqmid = (p->QualityIndex <= 2) ? (int)p->QualityIndex : LDACBT_EQMID_SQ;
    e->abr_enabled = (p->QualityIndex == PNA2DP_LDAC_EQMID_ABR);
    if (e->abr_enabled) e->eqmid = LDACBT_EQMID_SQ;
    /* libldac wants the full L2CAP MTU and subtracts the RTP + payload headers itself */
    int r = ldacBT_init_handle_encode(e->h, (int)(p->MediaMtu + 13), e->eqmid, LdacChannelMode(p->ChannelMode), e->fmt, (int)p->SampleRate);
    if (r != 0) { ldacBT_free_handle(e->h); free(e); return NULL; }
    e->pcm_per_frame = (p->SampleRate >= 88200) ? 256 : 128;
    /* ldacBT_encode always consumes one LSU of 128 PCM frames per call, at every sampling rate */
    e->frame_bytes = 128 * e->channels * e->bytes_per_sample;
    if (e->abr_enabled) {
        e->abr = ldac_ABR_get_handle();
        if (e->abr) { ldac_ABR_Init(e->abr, 1000); ldac_ABR_set_thresholds(e->abr, 6, 4, 2); }
    }
    return e;
}

static int QueuePacket(PnCodecEncoder *e, const uint8_t *data, uint32_t len, uint32_t frames)
{
    if (e->pending_count >= LDAC_MAX_PENDING || len > e->mtu || !frames || frames > 15) return -1;
    ldac_packet *q = &e->pending[(e->pending_head + e->pending_count) % LDAC_MAX_PENDING];
    memcpy(q->data, data, len);
    q->len = len;
    q->frames = frames;
    e->pending_count++;
    return 0;
}

static int PopPacket(PnCodecEncoder *e, PnCodecOutput *out)
{
    if (!e->pending_count) return 0;
    ldac_packet *q = &e->pending[e->pending_head];
    if (q->len > out->capacity) return -1;
    memcpy(out->buffer, q->data, q->len);
    out->written = q->len;
    out->frame_count = q->frames;
    out->pcm_frames_consumed = q->frames * e->pcm_per_frame;   /* RTP timestamp advance */
    out->payload_header_byte = q->frames & 0x0F;
    e->pending_head = (e->pending_head + 1) % LDAC_MAX_PENDING;
    e->pending_count--;
    return 1;
}

static int EncodeFrame(PnCodecEncoder *e, const uint8_t *frame)
{
    uint8_t packet[LDAC_MAX_PACKET];
    int pcm_used = 0, stream_wrote = 0, frame_num = 0;
    if (ldacBT_encode(e->h, (void *)frame, &pcm_used, packet, &stream_wrote, &frame_num) != 0 ||
        pcm_used != (int)e->frame_bytes || stream_wrote < 0) return -1;
    return stream_wrote ? QueuePacket(e, packet, (uint32_t)stream_wrote, (uint32_t)frame_num) : 0;
}

static int LdacEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    uint32_t stride = e->channels * e->bytes_per_sample;
    if (pcm_bytes % stride || pcm_bytes / stride != pcm_frames) return -1;
    uint32_t pos = 0;
    out->written = 0;
    out->frame_count = 0;
    out->pcm_frames_consumed = 0;

    /* complete a carried-over partial frame first */
    if (e->leftover_bytes) {
        uint32_t need = e->frame_bytes - e->leftover_bytes;
        uint32_t take = need < pcm_bytes ? need : pcm_bytes;
        memcpy(e->leftover + e->leftover_bytes, pcm, take);
        e->leftover_bytes += take;
        pos += take;
        if (e->leftover_bytes == e->frame_bytes) {
            if (EncodeFrame(e, e->leftover) != 0) return -1;
            e->leftover_bytes = 0;
        }
    }
    while (pos + e->frame_bytes <= pcm_bytes) {
        if (EncodeFrame(e, pcm + pos) != 0) return -1;
        pos += e->frame_bytes;
    }
    if (pos < pcm_bytes) {
        uint32_t rest = pcm_bytes - pos;
        if (rest <= sizeof(e->leftover)) { memcpy(e->leftover, pcm + pos, rest); e->leftover_bytes = rest; }
    }
    if (PopPacket(e, out) < 0) return -1;   /* zero-length output is legal: the driver sends nothing */
    out->actual_bitrate_bps = (uint32_t)ldacBT_get_bitrate(e->h) * 1000u;
    return 0;
}

static int LdacReconfigure(PnCodecEncoder *e, uint32_t quality, uint32_t bitrate)
{
    (void)bitrate;
    if (quality == PNA2DP_LDAC_EQMID_ABR) { e->abr_enabled = 1; return 0; }
    e->abr_enabled = 0;
    if (quality <= 2 && (int)quality != e->eqmid) {
        int result = ldacBT_set_eqmid(e->h, (int)quality);
        if (result == 0) e->eqmid = (int)quality;
        return result;
    }
    return 0;
}

static void LdacFlush(PnCodecEncoder *e)
{
    e->leftover_bytes = 0;
    e->pending_count = 0;
    e->pending_head = 0;
}

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
