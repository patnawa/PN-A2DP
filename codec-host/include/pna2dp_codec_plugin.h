/*
 * pna2dp_codec_plugin.h - plugin interface of the PN-A2DP user-mode codec host.
 *
 * A plugin is a DLL exporting `PnCodecPluginGetInfo`. The host loads every
 * `pna2dp-codec-*.dll` next to its executable (and from %ProgramData%\PN-A2DP\plugins),
 * asks for the codecs it provides and registers the union with the driver. Encoders
 * run in the host process: a plugin crash only takes the service down, never the kernel.
 *
 * Plugins are plain C so that LGPL libraries (libfreeaptx) and Apache libraries
 * (libldac) can be wrapped without touching the MIT-licensed driver.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "pna2dp_public.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PNA2DP_PLUGIN_API_VERSION  1

typedef struct PnCodecEncoder PnCodecEncoder;   /* opaque, per stream */

typedef struct PnCodecOutput {
    uint8_t  *buffer;            /* host-provided, capacity bytes */
    uint32_t  capacity;
    uint32_t  written;           /* encoded bytes produced */
    uint32_t  frame_count;       /* codec frames in buffer */
    uint32_t  pcm_frames_consumed;
    uint32_t  payload_header_byte;   /* value for the 1-byte media payload header (SBC/LDAC), 0 = default */
    uint32_t  actual_bitrate_bps;    /* informational */
} PnCodecOutput;

typedef struct PnCodecPlugin {
    uint32_t    api_version;         /* PNA2DP_PLUGIN_API_VERSION */
    const char *name;                /* "libldac", "libfreeaptx", ... */
    const char *version;
    const char *license;             /* SPDX identifier of the plugin + library */
    uint32_t    codec_mask;          /* PNA2DP_CODEC_BIT(...) */

    /* Create an encoder for the negotiated stream. Returns NULL on failure. */
    PnCodecEncoder *(*open)(const PNA2DP_STREAM_PARAMS *params);
    /* Encode `pcm_frames` interleaved frames in params->PcmFormat. Fill out. Return 0 on success. */
    int  (*encode)(PnCodecEncoder *enc, const uint8_t *pcm, uint32_t pcm_bytes, uint32_t pcm_frames, PnCodecOutput *out);
    /* Change quality/bitrate live (ABR). quality_index semantics are codec specific. */
    int  (*reconfigure)(PnCodecEncoder *enc, uint32_t quality_index, uint32_t bitrate_bps);
    void (*flush)(PnCodecEncoder *enc);
    void (*close)(PnCodecEncoder *enc);
    /* Optional: largest encoded block for the given params (0 = host default). */
    uint32_t (*max_output_bytes)(const PNA2DP_STREAM_PARAMS *params);
} PnCodecPlugin;

/* Exported by every plugin DLL. */
typedef const PnCodecPlugin *(*PnCodecPluginGetInfoFn)(void);
#define PNA2DP_PLUGIN_ENTRY "PnCodecPluginGetInfo"

#ifdef __cplusplus
}
#endif
