/*
 * PN-A2DP: open Bluetooth A2DP source driver for Windows.
 *
 * pna2dp_public.h - interface shared between the kernel driver (pna2dp.sys),
 * the user-mode codec host service and the control tools.
 *
 * Every structure is fixed layout, 8-byte packed, versioned by PNA2DP_ABI_VERSION.
 * All multi-byte integers are host endian. Strings are UTF-16, NUL terminated.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#ifndef _KERNEL_MODE
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>
#include <initguid.h>
#endif

/* Device interface class that every A2DP device node registers. One instance per
 * paired A2DP sink (source role) or source (sink role). The reference string is the
 * Bluetooth address as 12 hex digits. */
/* {5C9C9E8E-3B55-4B35-9D0C-4A5A6B4D2F01} */
DEFINE_GUID(GUID_DEVINTERFACE_PNA2DP,
    0x5c9c9e8e, 0x3b55, 0x4b35, 0x9d, 0x0c, 0x4a, 0x5a, 0x6b, 0x4d, 0x2f, 0x01);

#define PNA2DP_ABI_VERSION          1u

#define PNA2DP_DEVICE_TYPE          0x9A2Du  /* FILE_DEVICE_UNKNOWN range, vendor */

#define PNA2DP_CTL(code, access)    CTL_CODE(PNA2DP_DEVICE_TYPE, (code), METHOD_BUFFERED, (access))

/* Besides the FILE_*_ACCESS bits, the driver checks the caller role recorded when the control
 * handle was opened and fails other callers with ERROR_ACCESS_DENIED:
 *   GET_TRACE, SBC_SAMPLE  - elevated administrator or LocalSystem;
 *   CODEC_*                - LocalSystem only (the codec host service), and
 *                            CODEC_REGISTER.HostPid must be the calling process;
 *   everything else        - any caller (the control panel runs unelevated). */

/* ---- Status / configuration (control tools) ---------------------------------- */
#define IOCTL_PNA2DP_GET_VERSION        PNA2DP_CTL(0x800, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_GET_STATUS         PNA2DP_CTL(0x801, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_GET_CAPABILITIES   PNA2DP_CTL(0x802, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_GET_CONFIG         PNA2DP_CTL(0x803, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_SET_CONFIG         PNA2DP_CTL(0x804, FILE_WRITE_ACCESS)
#define IOCTL_PNA2DP_WAIT_EVENT         PNA2DP_CTL(0x805, FILE_READ_ACCESS)  /* pended */
#define IOCTL_PNA2DP_RECONNECT          PNA2DP_CTL(0x806, FILE_WRITE_ACCESS)
#define IOCTL_PNA2DP_GET_STATS          PNA2DP_CTL(0x807, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_GET_QUALITY        PNA2DP_CTL(0x80B, FILE_READ_ACCESS)
#define IOCTL_PNA2DP_RESET_STATS        PNA2DP_CTL(0x808, FILE_WRITE_ACCESS)
#define IOCTL_PNA2DP_GET_TRACE          PNA2DP_CTL(0x809, FILE_READ_ACCESS)
/* Explicit diagnostic capture: input ULONG 1 arms one SBC packet, 0 reads it, 2 cancels.
 * No audio is retained unless this write-access request is issued. */
#define IOCTL_PNA2DP_SBC_SAMPLE         PNA2DP_CTL(0x80A, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* ---- Codec host (inverted call) ----------------------------------------------- */
#define IOCTL_PNA2DP_CODEC_REGISTER     PNA2DP_CTL(0x900, FILE_WRITE_ACCESS)
#define IOCTL_PNA2DP_CODEC_UNREGISTER   PNA2DP_CTL(0x901, FILE_WRITE_ACCESS)
#define IOCTL_PNA2DP_CODEC_GET_WORK     PNA2DP_CTL(0x902, FILE_READ_ACCESS)   /* pended */
#define IOCTL_PNA2DP_CODEC_PUT_RESULT   PNA2DP_CTL(0x903, FILE_WRITE_ACCESS)

#pragma pack(push, 8)

/* Bounded, driver-wide diagnostic history. Text is NUL-terminated diagnostic text;
 * times are 100-ns units since boot. This read-only addition preserves ABI v1. */
#define PNA2DP_TRACE_ENTRIES 64u
#define PNA2DP_TRACE_TEXT_BYTES 192u
typedef struct _PNA2DP_TRACE_ENTRY {
    ULONGLONG TimeHns;
    ULONG Sequence;
    ULONG Level;
    CHAR Text[PNA2DP_TRACE_TEXT_BYTES];
} PNA2DP_TRACE_ENTRY;
typedef struct _PNA2DP_TRACE {
    ULONG Size;
    ULONG Count;
    PNA2DP_TRACE_ENTRY Entries[PNA2DP_TRACE_ENTRIES];
} PNA2DP_TRACE;

typedef struct _PNA2DP_SBC_SAMPLE {
    ULONG Size, SampleRate, Channels, GainQ15, Muted, PcmBytes, SbcBytes;
    UCHAR Pcm[7680];
    UCHAR Sbc[4096];
} PNA2DP_SBC_SAMPLE;

/* Codec identifiers. Values are stable; they double as bit positions in masks. */
typedef enum _PNA2DP_CODEC {
    PNA2DP_CODEC_NONE      = 0,
    PNA2DP_CODEC_SBC       = 1,   /* A2DP mandatory, 0x00 */
    PNA2DP_CODEC_AAC       = 2,   /* MPEG-2/4 AAC, 0x02 */
    PNA2DP_CODEC_APTX      = 3,   /* vendor 0x004F / 0x0001 */
    PNA2DP_CODEC_APTX_HD   = 4,   /* vendor 0x00D7 / 0x0024 */
    PNA2DP_CODEC_APTX_LL   = 5,   /* vendor 0x000A / 0x0002 */
    PNA2DP_CODEC_LDAC      = 6,   /* vendor 0x012D / 0x00AA */
    PNA2DP_CODEC_FASTSTREAM= 7,   /* vendor 0x000A / 0x0001 */
    PNA2DP_CODEC_OPUS      = 8,   /* vendor 0x05F1 / 0x1005 (PipeWire/Linux convention) */
    PNA2DP_CODEC_LC3PLUS   = 9,   /* vendor 0x08A9 / 0x0001 (Fraunhofer) */
    PNA2DP_CODEC_MAX
} PNA2DP_CODEC;

#define PNA2DP_CODEC_BIT(c)   (1u << (c))

/* A2DP SBC capability bits (A2DP spec 4.3.2), byte 0 and byte 1 layout. */
#define PNA2DP_SBC_FREQ_16000      0x80
#define PNA2DP_SBC_FREQ_32000      0x40
#define PNA2DP_SBC_FREQ_44100      0x20
#define PNA2DP_SBC_FREQ_48000      0x10
#define PNA2DP_SBC_CH_MONO         0x08
#define PNA2DP_SBC_CH_DUAL         0x04
#define PNA2DP_SBC_CH_STEREO       0x02
#define PNA2DP_SBC_CH_JOINT        0x01
#define PNA2DP_SBC_BLOCKS_4        0x80
#define PNA2DP_SBC_BLOCKS_8        0x40
#define PNA2DP_SBC_BLOCKS_12       0x20
#define PNA2DP_SBC_BLOCKS_16       0x10
#define PNA2DP_SBC_SUBBANDS_4      0x08
#define PNA2DP_SBC_SUBBANDS_8      0x04
#define PNA2DP_SBC_ALLOC_SNR       0x02
#define PNA2DP_SBC_ALLOC_LOUDNESS  0x01

/* Generic sample-rate mask used for every codec in this API. */
#define PNA2DP_RATE_8000      0x0001
#define PNA2DP_RATE_11025     0x0002
#define PNA2DP_RATE_12000     0x0004
#define PNA2DP_RATE_16000     0x0008
#define PNA2DP_RATE_22050     0x0010
#define PNA2DP_RATE_24000     0x0020
#define PNA2DP_RATE_32000     0x0040
#define PNA2DP_RATE_44100     0x0080
#define PNA2DP_RATE_48000     0x0100
#define PNA2DP_RATE_64000     0x0200
#define PNA2DP_RATE_88200     0x0400
#define PNA2DP_RATE_96000     0x0800

/* Generic channel-mode mask. */
#define PNA2DP_CHMODE_MONO    0x01
#define PNA2DP_CHMODE_DUAL    0x02
#define PNA2DP_CHMODE_STEREO  0x04
#define PNA2DP_CHMODE_JOINT   0x08

/* Generic sample-format mask. */
#define PNA2DP_FMT_S16        0x01
#define PNA2DP_FMT_S24        0x02
#define PNA2DP_FMT_S32        0x04
#define PNA2DP_FMT_F32        0x08

/* LDAC quality ladder (EQMID). */
#define PNA2DP_LDAC_EQMID_HQ   0   /* 990/909 kbps */
#define PNA2DP_LDAC_EQMID_SQ   1   /* 660/606 kbps */
#define PNA2DP_LDAC_EQMID_MQ   2   /* 330/303 kbps */
#define PNA2DP_LDAC_EQMID_ABR  0x7F

typedef struct _PNA2DP_VERSION {
    ULONG  AbiVersion;        /* PNA2DP_ABI_VERSION */
    ULONG  DriverVersion;     /* 0xMMmmbbbb */
    ULONG  Role;              /* 1 = source (PC sends), 2 = sink (PC receives) */
    ULONG  KernelCodecMask;   /* codecs encodable in-kernel (SBC) */
    ULONG  HostCodecMask;     /* codecs currently offered by a registered codec host */
    ULONG  Reserved[3];
} PNA2DP_VERSION, *PPNA2DP_VERSION;

typedef enum _PNA2DP_STATE {
    PNA2DP_STATE_IDLE = 0,       /* no AVDTP signalling channel */
    PNA2DP_STATE_CONNECTING,     /* L2CAP signalling opening */
    PNA2DP_STATE_DISCOVERING,    /* AVDTP DISCOVER / GET_CAPABILITIES */
    PNA2DP_STATE_CONFIGURED,     /* SET_CONFIGURATION done, media channel not open */
    PNA2DP_STATE_OPEN,           /* OPEN done, media channel connected, not streaming */
    PNA2DP_STATE_STREAMING,      /* START done */
    PNA2DP_STATE_SUSPENDED,      /* SUSPEND done (SCO, pause) */
    PNA2DP_STATE_CLOSING,
    PNA2DP_STATE_ERROR
} PNA2DP_STATE;

typedef struct _PNA2DP_STATUS {
    ULONG      Size;              /* sizeof(PNA2DP_STATUS) */
    ULONGLONG  RemoteAddress;     /* BTH_ADDR */
    WCHAR      RemoteName[64];
    ULONG      State;             /* PNA2DP_STATE */
    ULONG      Codec;             /* PNA2DP_CODEC in use */
    ULONG      SampleRate;
    ULONG      Channels;
    ULONG      BitsPerSample;     /* PCM bits the encoder consumes */
    ULONG      BitrateBps;        /* current encoded bitrate */
    ULONG      MaxBitrateBps;     /* configured ceiling (ABR) */
    ULONG      MediaMtu;          /* L2CAP out MTU of the media channel */
    ULONG      SinkReportedDelayUs; /* AVDTP delay report, 0 if none */
    ULONG      EstimatedLatencyUs;  /* buffer + encode + air + sink */
    ULONG      Flags;             /* PNA2DP_STATUS_FLAG_* */
    ULONG      LastError;         /* NTSTATUS of last failure */
    ULONG      AvdtpVersion;      /* 0x0103 for 1.3 */
    ULONG      RemoteSeid;
    ULONG      ActiveFormatIndex; /* index in the WaveRT format table */
    ULONG      EncoderOwner;      /* 0 none, 1 kernel, 2 host */
    /* Former Reserved[4] (0.1.0.22): size and offsets unchanged; older drivers report 0. */
    ULONG      EngineSampleRate;  /* Windows audio engine (WaveRT) rate while a stream is open, else 0;
                                     differs from SampleRate when the driver converts (RATE_CONVERTED) */
    ULONG      Reserved[3];
} PNA2DP_STATUS, *PPNA2DP_STATUS;

#define PNA2DP_STATUS_FLAG_SCO_ACTIVE        0x00000001
#define PNA2DP_STATUS_FLAG_ABR_ACTIVE        0x00000002
#define PNA2DP_STATUS_FLAG_DELAY_REPORTING   0x00000004
#define PNA2DP_STATUS_FLAG_REMOTE_INITIATED  0x00000008
#define PNA2DP_STATUS_FLAG_HOST_MISSING      0x00000010  /* host codec wanted, no host */
#define PNA2DP_STATUS_FLAG_FALLBACK_SBC      0x00000020
#define PNA2DP_STATUS_FLAG_AUDIO_ACTIVE      0x00000040  /* WaveRT pin in RUN */
#define PNA2DP_STATUS_FLAG_RADIO_IN_RANGE    0x00000080
#define PNA2DP_STATUS_FLAG_REMOTE_VOLUME     0x00000100  /* Windows confirmed receiver volume */
#define PNA2DP_STATUS_FLAG_REMOTE_SUSPENDED  0x00000200  /* the receiver suspended the stream */
#define PNA2DP_STATUS_FLAG_UNREACHABLE       0x00000400  /* link lost or media stalled; retrying */
#define PNA2DP_STATUS_FLAG_HALF_RATE         0x00000800  /* engine at twice SampleRate: the 2:1 decimator alone */
#define PNA2DP_STATUS_FLAG_RATE_CONVERTED    0x00001000  /* streaming through the sample-rate converter: EngineSampleRate != SampleRate */

/* One remote stream endpoint (SEP) as reported by AVDTP GET_(ALL_)CAPABILITIES. */
typedef struct _PNA2DP_REMOTE_SEP {
    UCHAR   Seid;
    UCHAR   InUse;
    UCHAR   MediaType;       /* 0 audio */
    UCHAR   TsepIsSink;      /* 1 = sink */
    ULONG   Codec;           /* PNA2DP_CODEC, or NONE if unknown vendor codec */
    ULONG   VendorId;        /* for vendor codecs */
    USHORT  VendorCodecId;
    USHORT  CodecInfoLength; /* bytes valid in CodecInfo (media codec capabilities) */
    UCHAR   CodecInfo[32];   /* raw Media Codec Specific Information Elements */
    ULONG   ServiceCaps;     /* PNA2DP_SVC_* flags */
    ULONG   Reserved;
} PNA2DP_REMOTE_SEP, *PPNA2DP_REMOTE_SEP;

#define PNA2DP_SVC_MEDIA_TRANSPORT   0x01
#define PNA2DP_SVC_REPORTING         0x02
#define PNA2DP_SVC_RECOVERY          0x04
#define PNA2DP_SVC_CONTENT_PROTECTION 0x08
#define PNA2DP_SVC_HEADER_COMPRESSION 0x10
#define PNA2DP_SVC_MULTIPLEXING      0x20
#define PNA2DP_SVC_DELAY_REPORTING   0x40

#define PNA2DP_MAX_REMOTE_SEPS   16

typedef struct _PNA2DP_CAPABILITIES {
    ULONG              Size;
    ULONG              SepCount;
    ULONG              CodecMask;          /* union of recognised codecs */
    ULONG              SdpSupportedFeatures; /* AudioSink SDP SupportedFeatures */
    ULONG              SdpProfileVersion;  /* 0x0103 */
    ULONG              Reserved[3];
    PNA2DP_REMOTE_SEP  Seps[PNA2DP_MAX_REMOTE_SEPS];
} PNA2DP_CAPABILITIES, *PPNA2DP_CAPABILITIES;

/* Per-codec desired parameters. A zero mask means "anything the sink supports,
 * prefer the best". */
typedef struct _PNA2DP_SBC_PARAMS {
    UCHAR   RateMask;        /* PNA2DP_SBC_FREQ_* */
    UCHAR   ChannelModeMask; /* PNA2DP_SBC_CH_* */
    UCHAR   BlockMask;       /* PNA2DP_SBC_BLOCKS_* */
    UCHAR   SubbandMask;     /* PNA2DP_SBC_SUBBANDS_* */
    UCHAR   AllocMask;       /* PNA2DP_SBC_ALLOC_* */
    UCHAR   MinBitpool;      /* 2..250 */
    UCHAR   MaxBitpool;      /* 2..250; > sink max only with AllowBitpoolOverride */
    UCHAR   AllowBitpoolOverride;
} PNA2DP_SBC_PARAMS;

typedef struct _PNA2DP_AAC_PARAMS {
    ULONG   RateMask;        /* PNA2DP_RATE_* */
    UCHAR   ChannelModeMask; /* MONO|STEREO */
    UCHAR   Vbr;             /* 1 = VBR allowed */
    UCHAR   ObjectType;      /* 0 auto, 2 = AAC-LC, 5 = HE-AACv1, 29 = HE-AACv2 */
    UCHAR   Afterburner;
    ULONG   BitrateBps;      /* 0 = sink max */
    ULONG   PeakBitrateBps;  /* VBR ceiling, 0 = same as Bitrate */
} PNA2DP_AAC_PARAMS;

typedef struct _PNA2DP_APTX_PARAMS {
    ULONG   RateMask;
    UCHAR   ChannelModeMask;
    UCHAR   FormatMask;      /* aptX HD: S16|S24 */
    UCHAR   LlTargetLevel;   /* aptX LL: target buffer level (frames), 0 = default */
    UCHAR   Reserved;
} PNA2DP_APTX_PARAMS;

typedef struct _PNA2DP_LDAC_PARAMS {
    ULONG   RateMask;        /* 44.1/48/88.2/96 */
    UCHAR   ChannelModeMask; /* MONO|DUAL|STEREO */
    UCHAR   FormatMask;      /* S16|S24|S32|F32 (encoder input) */
    UCHAR   Eqmid;           /* PNA2DP_LDAC_EQMID_* */
    UCHAR   AbrEnable;
} PNA2DP_LDAC_PARAMS;

typedef struct _PNA2DP_CONFIG {
    ULONG   Size;
    ULONG   Version;             /* PNA2DP_ABI_VERSION */
    ULONG   CodecPriority[8];    /* ordered PNA2DP_CODEC list; 0 terminates */
    ULONG   EnabledCodecMask;    /* codecs the user allows */
    ULONG   Flags;               /* PNA2DP_CFG_* */
    ULONG   TargetLatencyMs;     /* media buffer target (default 60) */
    ULONG   MaxLatencyMs;        /* hard ceiling before dropping (default 250) */
    ULONG   AbrFloorBps;         /* ABR will not go below */
    LONG    VolumeDb16;          /* persisted endpoint volume, 16.16 dB */
    ULONG   ReconnectDelayMs;
    PNA2DP_SBC_PARAMS  Sbc;
    PNA2DP_AAC_PARAMS  Aac;
    PNA2DP_APTX_PARAMS Aptx;
    PNA2DP_APTX_PARAMS AptxHd;
    PNA2DP_APTX_PARAMS AptxLl;
    PNA2DP_LDAC_PARAMS Ldac;
    ULONG   Reserved[8];
} PNA2DP_CONFIG, *PPNA2DP_CONFIG;

#define PNA2DP_CFG_ABR_ENABLE            0x00000001
#define PNA2DP_CFG_APPLY_LIVE            0x00000002  /* reconfigure without disconnect */
#define PNA2DP_CFG_SUSPEND_ON_SCO        0x00000004
#define PNA2DP_CFG_AUTO_START_ON_CONNECT 0x00000008  /* START immediately after OPEN */
#define PNA2DP_CFG_EXPOSE_ALL_RATES      0x00000010  /* advertise every rate (exclusive apps) */
#define PNA2DP_CFG_SOFTWARE_VOLUME       0x00000020  /* always enabled until AVRCP volume is implemented */
#define PNA2DP_CFG_DELAY_REPORTING       0x00000040  /* accept AVDTP delay reports */
#define PNA2DP_CFG_DUAL_CHANNEL_PREFER   0x00000080  /* SBC: prefer dual channel over joint */
#define PNA2DP_CFG_HOST_CODECS_ONLY      0x00000100  /* never fall back to kernel SBC */
#define PNA2DP_CFG_AUTO_QUALITY          0x00000200  /* adapter-aware limits + live ABR */
#define PNA2DP_CFG_SBC_XQ                0x00000400  /* SBC: dual channel, 16 blocks, 8 subbands at the bitpool
                                                        plain dual channel gets, packed to the EDR packets
                                                        both ends take; default off */

/* Optional read-only extension: older ABI-1 drivers may reject GET_QUALITY. */
#define PNA2DP_QUALITY_RADIO_VALID       0x00000001
#define PNA2DP_QUALITY_AUTO              0x00000002
#define PNA2DP_QUALITY_ABR               0x00000004
#define PNA2DP_QUALITY_CONSERVATIVE      0x00000008
#define PNA2DP_QUALITY_CHANGE_DETAILS   0x00000010
#define PNA2DP_QUALITY_REASON_SLOW      0x00000001
#define PNA2DP_QUALITY_REASON_FAILED    0x00000002
#define PNA2DP_QUALITY_REASON_PRESSURE  0x00000004
#define PNA2DP_QUALITY_REASON_BACKLOG   0x00000008
#define PNA2DP_QUALITY_REASON_RECOVERY  0x00000010
#define PNA2DP_QUALITY_IDLE              0
#define PNA2DP_QUALITY_SAMPLING          1
#define PNA2DP_QUALITY_STABLE            2
#define PNA2DP_QUALITY_CONGESTED         3
#define PNA2DP_QUALITY_RECOVERING        4
typedef struct _PNA2DP_QUALITY {
    ULONG Size;
    ULONG Flags;
    ULONGLONG LocalAddress;
    ULONGLONG LmpFeatures;       /* adapter capability, not negotiated link speed */
    ULONG RadioQueryStatus;     /* NTSTATUS; failure means unknown */
    USHORT Manufacturer;
    USHORT HciRevision;
    USHORT LmpSubversion;
    UCHAR HciVersion;
    UCHAR LmpVersion;
    ULONG Health;
    ULONG Codec;
    ULONG BitrateBps;
    ULONG CeilingBps;
    ULONG FloorBps;
    ULONG Level;
    ULONG MaxLevel;
    ULONG HealthyMs;
    ULONG CongestedMs;
    ULONG CompletedPackets;    /* latest observation window */
    ULONG SlowPackets;
    ULONG FailedPackets;
    ULONG BacklogPeakUs;
    ULONG ObservationMs;
    /* Former reserved words: structure size and ABI 1 remain unchanged.
     * Read only with CHANGE_DETAILS; older drivers leave these words zero. */
    ULONG RecoveryTargetMs;
    ULONG ChangeSequence;
    ULONG LastChangeReason;
    ULONG Reserved;
} PNA2DP_QUALITY;

/* Event mask returned by IOCTL_PNA2DP_WAIT_EVENT (output ULONG). */
#define PNA2DP_EVENT_STATE_CHANGED       0x00000001
#define PNA2DP_EVENT_CAPS_CHANGED        0x00000002
#define PNA2DP_EVENT_CONFIG_CHANGED      0x00000004
#define PNA2DP_EVENT_STATS_TICK          0x00000008
#define PNA2DP_EVENT_SCO_CHANGED         0x00000010
#define PNA2DP_EVENT_HOST_CHANGED        0x00000020
#define PNA2DP_EVENT_DELAY_REPORT        0x00000040
#define PNA2DP_EVENT_ERROR               0x00000080

typedef struct _PNA2DP_STATS {
    ULONG      Size;
    ULONGLONG  PcmFramesConsumed;   /* PCM frames taken from the WaveRT ring (engine rate) */
    ULONGLONG  EncodedFrames;
    ULONGLONG  PacketsSent;
    ULONGLONG  BytesSent;
    ULONGLONG  PacketsDropped;      /* dropped by ABR / latency guard */
    ULONGLONG  WriteFailures;
    ULONGLONG  Underruns;           /* latency-guard trims: buffered PCM discarded to bound latency
                                       (historical name; WaveRT starvation is WaveStarvedPackets) */
    ULONG      QueueDepthPackets;   /* outstanding L2CAP writes */
    ULONG      QueueDepthMaxPackets;
    ULONG      TxCompletionAvgUs;   /* moving average L2CAP write completion time */
    ULONG      TxCompletionMaxUs;
    ULONG      AbrLevel;            /* codec-specific ladder index, 0 = best */
    ULONG      AbrDowngrades;
    ULONG      AbrUpgrades;
    ULONG      HostRoundTripAvgUs;  /* codec host encode round trip */
    ULONG      HostTimeouts;
    ULONG      TransportWaits;      /* retained PCM/results while TX queue was full */
    ULONG      WaveLateTicks;       /* timer callbacks later than a notification period */
    ULONG      WaveMaxTickUs;
    /* Former Reserved[4]: size and offsets unchanged; older drivers report 0. */
    ULONG      WaveStarvedPackets;  /* packets the WaveRT side had no new PCM for */
    ULONG      RingOverflowEvents;  /* PCM ring overruns: oldest audio discarded (events, not bytes) */
    ULONG      FragmentedPackets;   /* AAC access units that needed more than one RTP packet */
    ULONG      HostDroppedBlocks;   /* encode blocks lost at the codec host (bad, invalid or unanswered) */
} PNA2DP_STATS, *PPNA2DP_STATS;

/* ---- Codec host protocol ----------------------------------------------------- */

/* The host registers once per device with the codec mask it can encode and the
 * number of work buffers it keeps pended. The driver then completes GET_WORK requests
 * with PCM blocks and expects PUT_RESULT with the encoded payload for the same
 * sequence number. */
typedef struct _PNA2DP_CODEC_REGISTER {
    ULONG   Size;
    ULONG   AbiVersion;
    ULONG   CodecMask;         /* PNA2DP_CODEC_BIT(...) the host supports */
    ULONG   MaxPcmBytes;       /* largest PCM block the host accepts per work item */
    ULONG   MaxEncodedBytes;   /* largest payload per result */
    ULONG   HostPid;
    ULONG   Reserved[2];
} PNA2DP_CODEC_REGISTER;

typedef enum _PNA2DP_WORK_KIND {
    PNA2DP_WORK_OPEN = 1,     /* create encoder with the given stream params */
    PNA2DP_WORK_ENCODE = 2,   /* encode PCM */
    PNA2DP_WORK_RECONFIG = 3, /* bitrate / quality change (ABR) */
    PNA2DP_WORK_CLOSE = 4,
    PNA2DP_WORK_FLUSH = 5
} PNA2DP_WORK_KIND;

typedef struct _PNA2DP_STREAM_PARAMS {
    ULONG   Codec;             /* PNA2DP_CODEC */
    ULONG   SampleRate;
    ULONG   Channels;          /* 1 or 2 */
    ULONG   PcmFormat;         /* PNA2DP_FMT_* exactly one */
    ULONG   ChannelMode;       /* PNA2DP_CHMODE_* exactly one */
    ULONG   BitrateBps;        /* target */
    ULONG   MediaMtu;          /* L2CAP MTU, payload must fit RTP(12)+hdr */
    ULONG   QualityIndex;      /* codec ladder index (LDAC EQMID, SBC bitpool, ...) */
    UCHAR   CodecInfo[32];     /* negotiated AVDTP codec IEs */
    ULONG   CodecInfoLength;
    ULONG   Reserved[4];
} PNA2DP_STREAM_PARAMS;

typedef struct _PNA2DP_CODEC_WORK {
    ULONG   Size;              /* total bytes incl. trailing PCM */
    ULONG   Kind;              /* PNA2DP_WORK_KIND */
    ULONG   Sequence;          /* echoed in result */
    ULONG   StreamGeneration;  /* increments on every OPEN */
    PNA2DP_STREAM_PARAMS Params;
    ULONG   PcmBytes;          /* bytes of PCM following this header */
    ULONG   PcmFrames;
    ULONG   Reserved[2];
    /* UCHAR Pcm[PcmBytes]; */
} PNA2DP_CODEC_WORK;

typedef struct _PNA2DP_CODEC_RESULT {
    ULONG   Size;              /* total bytes incl. trailing payload */
    ULONG   Sequence;
    ULONG   StreamGeneration;
    LONG    Status;            /* 0 ok, else host error */
    ULONG   PayloadBytes;      /* encoded bytes following this header */
    ULONG   FrameCount;        /* codec frames in payload (RTP header hints) */
    ULONG   PcmFramesConsumed; /* for clock accounting */
    ULONG   PayloadHeaderByte; /* e.g. SBC media payload header frame count byte */
    ULONG   ActualBitrateBps;
    ULONG   Reserved[3];
    /* UCHAR Payload[PayloadBytes]; */
} PNA2DP_CODEC_RESULT;

#pragma pack(pop)
