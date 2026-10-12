/*
 * aptx_plugin.c - aptX / aptX HD / aptX Low Latency plugin using libfreeaptx (LGPL-2.1+).
 *
 * Output DLL: pna2dp-codec-aptx.dll. libfreeaptx is linked as its own DLL
 * (freeaptx.dll) so the LGPL obligations are met by replaceability.
 *
 * Framing (matches Android/BlueZ/PipeWire):
 *   aptX     : no RTP header, 4 PCM frames -> 8 bytes (6 bytes/frame of 24-bit stereo in)
 *   aptX HD  : RTP header, no payload header, 4 PCM frames -> 12 bytes
 *   aptX LL  : no RTP header, same framing as aptX; the sink expects small packets
 * The driver already decides RTP header use; the plugin only produces raw codec bytes.
 *
 * SPDX-License-Identifier: MIT (plugin); libfreeaptx is LGPL-2.1-or-later
 */
#include "../../include/pna2dp_codec_plugin.h"
#include "freeaptx.h"
#include <stdlib.h>
#include <string.h>

struct PnCodecEncoder {
    struct aptx_context *ctx;
    int hd;
    uint32_t channels;
    uint32_t mtu;
    uint32_t block_bytes;     /* 8 or 12 */
};

static PnCodecEncoder *AptxOpen(const PNA2DP_STREAM_PARAMS *p)
{
    if (p->Codec != PNA2DP_CODEC_APTX && p->Codec != PNA2DP_CODEC_APTX_HD && p->Codec != PNA2DP_CODEC_APTX_LL) return NULL;
    if (p->PcmFormat != PNA2DP_FMT_S24 || p->Channels != 2) return NULL;   /* libfreeaptx: 24-bit stereo, 3 bytes per sample little endian */
    if (p->MediaMtu < (p->Codec == PNA2DP_CODEC_APTX_HD ? 12u : 8u)) return NULL;
    PnCodecEncoder *e = (PnCodecEncoder *)calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->hd = (p->Codec == PNA2DP_CODEC_APTX_HD);
    e->ctx = aptx_init(e->hd);
    if (!e->ctx) { free(e); return NULL; }
    e->channels = 2;
    e->mtu = p->MediaMtu;
    e->block_bytes = e->hd ? 12 : 8;
    return e;
}

static int AptxEncode(PnCodecEncoder *e, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out)
{
    size_t written = 0;
    uint32_t blocks = pcm_frames / 4;
    uint32_t max_blocks = e->mtu / e->block_bytes;
    if (pcm_frames % 4 || blocks > max_blocks || blocks > out->capacity / e->block_bytes ||
        pcm_bytes / 6 != pcm_frames || pcm_bytes % 6) return -1;
    size_t in_bytes = (size_t)blocks * 4 * e->channels * 3;
    size_t consumed = aptx_encode(e->ctx, pcm, in_bytes, out->buffer, out->capacity, &written);
    if (consumed != in_bytes) return -1; // the driver cannot re-feed a partially consumed block
    out->written = (uint32_t)written;
    out->frame_count = (uint32_t)(written / e->block_bytes);
    out->pcm_frames_consumed = (uint32_t)(consumed / (3 * e->channels));
    out->payload_header_byte = 0;
    out->actual_bitrate_bps = 0;
    return 0;
}

static int AptxReconfigure(PnCodecEncoder *e, uint32_t q, uint32_t b) { (void)e; (void)q; (void)b; return 0; }
static void AptxFlush(PnCodecEncoder *e) { aptx_reset(e->ctx); }
static void AptxClose(PnCodecEncoder *e) { aptx_finish(e->ctx); free(e); }
static uint32_t AptxMaxOut(const PNA2DP_STREAM_PARAMS *p) { return p->MediaMtu + 16; }

static const PnCodecPlugin g_Plugin = {
    PNA2DP_PLUGIN_API_VERSION, "libfreeaptx", "0.2.2", "LGPL-2.1-or-later",
    PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL),
    AptxOpen, AptxEncode, AptxReconfigure, AptxFlush, AptxClose, AptxMaxOut
};

__declspec(dllexport) const PnCodecPlugin *PnCodecPluginGetInfo(void) { return &g_Plugin; }
