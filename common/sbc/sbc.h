/*
 * sbc.h - Low-complexity Subband Codec (Bluetooth A2DP specification, Appendix B).
 *
 * Portable, allocation-free, integer-only implementation usable from the Windows
 * kernel (pna2dp.sys) and from user mode (codec host, unit tests). Encoder and decoder.
 *
 * Coefficient tables are the values printed in the A2DP specification (sections
 * 12.8 and 12.9), scaled to Q30 fixed point.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PN_SBC_H
#define PN_SBC_H

#ifdef __cplusplus
extern "C" {
#endif

typedef signed char        sbc_s8;
typedef unsigned char      sbc_u8;
typedef short              sbc_s16;
typedef unsigned short     sbc_u16;
typedef int                sbc_s32;
typedef unsigned int       sbc_u32;
typedef long long          sbc_s64;
typedef unsigned long long sbc_u64;

#define SBC_MAX_SUBBANDS   8
#define SBC_MAX_BLOCKS     16
#define SBC_MAX_CHANNELS   2
#define SBC_MAX_FRAME_LEN  1024                                  /* bytes, generous */
#define SBC_MAX_PCM_FRAME  (SBC_MAX_SUBBANDS * SBC_MAX_BLOCKS)   /* samples per channel */

#define SBC_SYNCWORD       0x9C
#define SBC_MSBC_SYNCWORD  0xAD

enum sbc_channel_mode { SBC_MODE_MONO = 0, SBC_MODE_DUAL = 1, SBC_MODE_STEREO = 2, SBC_MODE_JOINT = 3 };
enum sbc_allocation   { SBC_ALLOC_LOUDNESS = 0, SBC_ALLOC_SNR = 1 };

typedef struct sbc_params {
    sbc_u32 sample_rate;     /* 16000, 32000, 44100, 48000 */
    sbc_u8  channel_mode;    /* enum sbc_channel_mode */
    sbc_u8  allocation;      /* enum sbc_allocation */
    sbc_u8  subbands;        /* 4 or 8 */
    sbc_u8  blocks;          /* 4, 8, 12, 16 */
    sbc_u8  bitpool;         /* 2..250 */
} sbc_params;

typedef struct sbc_encoder {
    sbc_params p;
    sbc_u8  freq_index;      /* 0..3 */
    sbc_u8  channels;        /* 1 or 2 */
    sbc_u32 frame_length;    /* bytes */
    sbc_u32 pcm_per_frame;   /* PCM frames per SBC frame (subbands*blocks) */
    /* analysis delay line per channel: 80 samples, newest at index 0, Q8 in the 16-bit
     * domain (24-bit precision; 16-bit input is stored << 8) */
    sbc_s32 x[SBC_MAX_CHANNELS][80];
    /* scratch */
    sbc_s32 sb[SBC_MAX_BLOCKS][SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];   /* Q15 subband samples */
    sbc_u8  scale_factor[SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];
    sbc_u8  bits[SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];
    sbc_u8  join[SBC_MAX_SUBBANDS];
} sbc_encoder;

typedef struct sbc_decoder {
    sbc_params p;
    sbc_u8  freq_index;
    sbc_u8  channels;
    sbc_u32 frame_length;
    /* synthesis state per channel: V[160] (newest at index 0) */
    sbc_s32 v[SBC_MAX_CHANNELS][160];
    sbc_s32 sb[SBC_MAX_BLOCKS][SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];
    sbc_u8  scale_factor[SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];
    sbc_u8  bits[SBC_MAX_CHANNELS][SBC_MAX_SUBBANDS];
    sbc_u8  join[SBC_MAX_SUBBANDS];
} sbc_decoder;

/* ---- parameter helpers ---- */
sbc_u32 sbc_frame_length(const sbc_params *p);         /* bytes; 0 if params invalid */
sbc_u32 sbc_bitrate(const sbc_params *p);              /* bits per second */
sbc_u32 sbc_pcm_per_frame(const sbc_params *p);        /* PCM frames per SBC frame */
int     sbc_params_valid(const sbc_params *p);
/* Largest bitpool whose frame fits in max_frame_bytes (0 = unconstrained) and whose
 * bitrate does not exceed max_bitrate (0 = unconstrained). */
sbc_u8  sbc_max_bitpool_for(const sbc_params *p, sbc_u32 max_bitrate, sbc_u32 max_frame_bytes);

/* ---- encoder ---- */
int     sbc_encoder_init(sbc_encoder *e, const sbc_params *p);   /* 0 ok, -1 bad params */
void    sbc_encoder_set_bitpool(sbc_encoder *e, sbc_u8 bitpool); /* live change, 2..250 */
/* Encode exactly e->pcm_per_frame interleaved S16 PCM frames (e->channels channels).
 * Returns bytes written (== e->frame_length) or 0 if out_cap is too small. */
sbc_u32 sbc_encode_frame(sbc_encoder *e, const sbc_s16 *pcm, sbc_u8 *out, sbc_u32 out_cap);
/* Same, from interleaved S32 PCM (full scale +-2^31). The top 24 bits reach the analysis
 * filter, so gain applied before encoding and 24-bit sources keep their precision.
 * S16 samples passed as (s16 << 16) produce exactly the frames sbc_encode_frame does. */
sbc_u32 sbc_encode_frame_s32(sbc_encoder *e, const sbc_s32 *pcm, sbc_u8 *out, sbc_u32 out_cap);

/* ---- decoder ---- */
int     sbc_decoder_init(sbc_decoder *d);
/* Decodes one frame. Returns bytes consumed, 0 on malformed input (resync needed).
 * Writes interleaved S16 PCM (channels * pcm_per_frame samples) and fills *pcm_frames. */
sbc_u32 sbc_decode_frame(sbc_decoder *d, const sbc_u8 *in, sbc_u32 in_len, sbc_s16 *pcm, sbc_u32 pcm_cap_samples,
                         sbc_u32 *pcm_frames, sbc_u32 *channels, sbc_u32 *sample_rate);
/* Parses a frame header without decoding; returns frame length (0 if invalid). */
sbc_u32 sbc_parse_header(const sbc_u8 *in, sbc_u32 in_len, sbc_params *p);

#ifdef __cplusplus
}
#endif
#endif /* PN_SBC_H */
