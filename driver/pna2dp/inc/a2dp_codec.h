/*
 * a2dp_codec.h - A2DP codec capability model.
 *
 * Parses the Media Codec capability IEs of every known codec into a neutral
 * description, picks the best configuration against user preferences, and builds the
 * configuration IEs for SET_CONFIGURATION / RECONFIGURE.
 *
 * IE layouts (A2DP 1.3 + vendor specifications as implemented by Android/BlueZ):
 *   SBC   (0x00): [freq|chmode][blocks|subbands|alloc][minbp][maxbp]
 *   AAC   (0x02): [objtype][freq hi][freq lo(4)|ch(2)|rfa][vbr|bitrate 23 bits]
 *   vendor(0xFF): [vendor id LE32][codec id LE16][codec specific...]
 *     aptX     004F/0001: [freq(4)|chmode(4)]
 *     aptX HD  00D7/0024: [freq(4)|chmode(4)][4 x rfa]
 *     aptX LL  000A/0002: [freq(4)|chmode(4)][bidirect|rfa][optional: target/initial/sra params]
 *     LDAC     012D/00AA: [freq][chmode]
 *     FastStream 000A/0001: [direction][frequency]
 *     Opus     05F1/1005: see PipeWire OPUS-A2DP spec (we support the simple stereo profile)
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define A2DP_CODEC_SBC          0x00
#define A2DP_CODEC_MPEG12       0x01
#define A2DP_CODEC_MPEG24_AAC   0x02
#define A2DP_CODEC_ATRAC        0x04
#define A2DP_CODEC_VENDOR       0xFF

#define A2DP_VENDOR_APTX        0x0000004Fu
#define A2DP_VENDOR_APTX_HD     0x000000D7u
#define A2DP_VENDOR_CSR         0x0000000Au   /* aptX LL, FastStream */
#define A2DP_VENDOR_SONY        0x0000012Du   /* LDAC */
#define A2DP_VENDOR_LINUXFOUND  0x000005F1u   /* Opus (PipeWire) */
#define A2DP_VENDOR_FRAUNHOFER  0x000008A9u   /* LC3plus */

#define A2DP_CODECID_APTX       0x0001
#define A2DP_CODECID_APTX_HD    0x0024
#define A2DP_CODECID_APTX_LL    0x0002
#define A2DP_CODECID_FASTSTREAM 0x0001
#define A2DP_CODECID_LDAC       0x00AA
#define A2DP_CODECID_OPUS       0x1005
#define A2DP_CODECID_LC3PLUS    0x0001

/* AAC IE bits */
#define AAC_OBJ_MPEG2_LC   0x80
#define AAC_OBJ_MPEG4_LC   0x40
#define AAC_OBJ_MPEG4_LTP  0x20
#define AAC_OBJ_MPEG4_SCA  0x10
#define AAC_OBJ_HE_V1      0x08
#define AAC_OBJ_HE_V2      0x04
#define AAC_OBJ_ELD        0x02
/* AAC frequency: byte1 bits7..0 = 8k,11.025k,12k,16k,22.05k,24k,32k,44.1k ; byte2 bits7..4 = 48k,64k,88.2k,96k */
#define AAC_CH_MONO        0x08
#define AAC_CH_STEREO      0x04
#define AAC_CH_6           0x02   /* 5.1 (A2DP 1.4) */
#define AAC_CH_8           0x01

/* aptX family IE bits (byte 0) */
#define APTX_FREQ_16000   0x80
#define APTX_FREQ_32000   0x40
#define APTX_FREQ_44100   0x20
#define APTX_FREQ_48000   0x10
#define APTX_CH_MONO      0x01
#define APTX_CH_STEREO    0x02
/* aptX HD/LL: same byte 0 layout */

/* LDAC IE bits */
#define LDAC_FREQ_44100   0x20
#define LDAC_FREQ_48000   0x10
#define LDAC_FREQ_88200   0x08
#define LDAC_FREQ_96000   0x04
#define LDAC_FREQ_176400  0x02
#define LDAC_FREQ_192000  0x01
#define LDAC_CH_MONO      0x04
#define LDAC_CH_DUAL      0x02
#define LDAC_CH_STEREO    0x01

/* FastStream IE */
#define FASTSTREAM_DIR_SINK   0x01   /* 48 kHz forward (we send) */
#define FASTSTREAM_DIR_SOURCE 0x02   /* 16 kHz back channel (mic) */
#define FASTSTREAM_SINK_FREQ_48000 0x01
#define FASTSTREAM_SINK_FREQ_44100 0x02
#define FASTSTREAM_SOURCE_FREQ_16000 0x20

/* Neutral representation of what a SEP (or we) can do with a codec. */
typedef struct _A2DP_CODEC_CAPS {
    ULONG   Codec;             /* PNA2DP_CODEC */
    ULONG   RateMask;          /* PNA2DP_RATE_* */
    ULONG   ChannelModeMask;   /* PNA2DP_CHMODE_* */
    ULONG   FormatMask;        /* PNA2DP_FMT_* (bit depth the codec carries) */
    /* SBC */
    UCHAR   SbcBlockMask, SbcSubbandMask, SbcAllocMask, SbcMinBitpool, SbcMaxBitpool;
    /* AAC */
    UCHAR   AacObjectMask;     /* AAC_OBJ_* */
    BOOLEAN AacVbr;
    ULONG   AacMaxBitrate;
    /* aptX LL */
    BOOLEAN AptxLlBidirectional;
    /* FastStream */
    UCHAR   FsDirections;
} A2DP_CODEC_CAPS, *PA2DP_CODEC_CAPS;

/* A concrete, single-valued configuration (what goes into SET_CONFIGURATION). */
typedef struct _A2DP_CODEC_CONFIG {
    ULONG   Codec;             /* PNA2DP_CODEC */
    ULONG   SampleRate;
    ULONG   Channels;          /* 1 or 2 */
    ULONG   ChannelMode;       /* PNA2DP_CHMODE_* single bit */
    ULONG   PcmFormat;         /* PNA2DP_FMT_* single bit (encoder input) */
    ULONG   BitrateBps;        /* target / nominal */
    ULONG   MaxBitrateBps;     /* ceiling for ABR */
    /* SBC */
    UCHAR   SbcBlocks;         /* 4,8,12,16 */
    UCHAR   SbcSubbands;       /* 4,8 */
    UCHAR   SbcAlloc;          /* 0 loudness, 1 snr */
    UCHAR   SbcMinBitpool;
    UCHAR   SbcMaxBitpool;
    UCHAR   SbcBitpool;        /* current (ABR) */
    /* AAC */
    UCHAR   AacObjectType;     /* 2 LC, 5 HEv1, 29 HEv2 */
    UCHAR   AacObjectMask;     /* wire object bit; MPEG-2 LC and MPEG-4 LC both use AOT 2 */
    BOOLEAN AacVbr;
    /* LDAC */
    UCHAR   LdacEqmid;
    /* aptX LL */
    UCHAR   AptxLlTargetLevel;
    /* Raw IEs as sent on the wire (media codec specific information elements) */
    UCHAR   Ies[32];
    UCHAR   IesLength;
    UCHAR   MediaCodecType;    /* A2DP_CODEC_* */
} A2DP_CODEC_CONFIG, *PA2DP_CODEC_CONFIG;

/* Parse the capability IEs of a remote SEP (media codec type + IEs) into Caps.
 * Returns FALSE for unknown codecs. */
BOOLEAN A2dpParseCaps(_In_ UCHAR MediaCodecType, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                      _Out_ PA2DP_CODEC_CAPS Caps, _Out_opt_ PULONG VendorId, _Out_opt_ PUSHORT VendorCodecId);

/* Parse a *configuration* (single-valued IEs) into a config structure. */
BOOLEAN A2dpParseConfig(_In_ UCHAR MediaCodecType, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                        _Out_ PA2DP_CODEC_CONFIG Cfg);

/* Build the capability IEs we advertise for a codec (our local SEP). */
ULONG   A2dpBuildLocalCaps(_In_ ULONG Codec, _Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap,
                           _Out_ PUCHAR MediaCodecType);

/* Choose a configuration for Codec from remote Caps using user preferences and the
 * preferred engine sample rate. Fills Cfg and its wire IEs. */
BOOLEAN A2dpSelectConfig(_In_ const A2DP_CODEC_CAPS *Caps, _In_ const PNA2DP_CONFIG *Pref,
                         _In_ ULONG PreferredRate, _In_ ULONG PreferredBits, _Out_ PA2DP_CODEC_CONFIG Cfg);

/* Re-serialise Cfg into Cfg->Ies (after an ABR / live parameter change). */
VOID    A2dpSerializeConfig(_Inout_ PA2DP_CODEC_CONFIG Cfg);
BOOLEAN A2dpConstrainBasicRadio(_Inout_ PA2DP_CODEC_CONFIG Cfg);

/* Nominal bitrate for a config (SBC computed from bitpool, others from tables). */
ULONG   A2dpNominalBitrate(_In_ const A2DP_CODEC_CONFIG *Cfg);

/* Map helpers */
ULONG   A2dpRateMaskToHz(_In_ ULONG SingleBit);
ULONG   A2dpHzToRateMask(_In_ ULONG Hz);
const char *A2dpCodecName(_In_ ULONG Codec);

ULONG A2dpQualityAtLevel(_In_ const A2DP_CODEC_CONFIG *Base, ULONG Level, ULONG FloorBps, _Out_opt_ A2DP_CODEC_CONFIG *Result);
