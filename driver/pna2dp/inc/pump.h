/*
 * pump.h - the streaming engine.
 *
 *   WaveRT timer (DISPATCH)  --PCM bytes-->  PCM ring  --> pump thread (PASSIVE, high prio)
 *                                                         |-> kernel SBC encoder
 *                                                         |-> user-mode codec host (inverted call)
 *                                                         --> RTP packetiser --> L2CAP media channel
 *
 * The WaveRT side is the clock. The pump drains whatever PCM is available, converts it to
 * the encoder's input format, encodes, and keeps a bounded number of L2CAP writes in
 * flight. A latency guard drops PCM when the ring exceeds MaxLatencyMs; the adaptive
 * bitrate controller (ABR) watches write completion latency and queue depth.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "audio_policy.h"

#define PN_PUMP_RING_MS            600     /* PCM ring capacity */
#define PN_PUMP_MAX_PACKET         2048    /* staging buffer per RTP packet */
#define PN_PUMP_MAX_PCM_BLOCK      16384   /* bytes handed to an encoder at once */
#define PN_PUMP_HOST_WINDOW        4       /* outstanding encode blocks at the host */
#define PN_PUMP_HOST_TIMEOUT_MS    200
#define PN_PUMP_DEFAULT_TARGET_MS  60
#define PN_PUMP_DEFAULT_MAX_MS     250

typedef enum _PN_ENCODER_OWNER {
    PnEncoderNone = 0,
    PnEncoderKernelSbc = 1,
    PnEncoderHost = 2
} PN_ENCODER_OWNER;

typedef struct _PN_PCM_RING {
    PUCHAR      Buffer;
    ULONG       Capacity;       /* bytes */
    ULONGLONG   WritePos;       /* monotonically increasing byte counters */
    ULONGLONG   ReadPos;
    KSPIN_LOCK  Lock;
    ULONG       BytesPerFrame;  /* input (WaveRT) frame size */
    ULONG       SampleRate;
    ULONG       Channels;
    ULONG       BitsPerSample;  /* container bits of the WaveRT format */
    ULONG       ValidBits;
    BOOLEAN     IsFloat;
    ULONGLONG   DroppedBytes;
} PN_PCM_RING, *PPN_PCM_RING;

/* A unit of encoded output waiting for the RTP packetiser (host path). */
typedef struct _PN_ENCODED_BLOCK {
    LIST_ENTRY  Link;
    ULONG       Sequence;
    ULONG       PayloadBytes;
    ULONG       FrameCount;
    ULONG       PcmFrames;
    ULONG       PayloadHeaderByte;
    ULONG       ActualBitrate;
    LONGLONG    SubmitTimeHns;
    UCHAR       Payload[PN_PUMP_MAX_PACKET];
} PN_ENCODED_BLOCK, *PPN_ENCODED_BLOCK;

typedef struct _PN_ABR {
    BOOLEAN     Enabled;
    ULONG       Level;             /* 0 = best quality */
    ULONG       MaxLevel;
    PN_QUALITY_WINDOW Window;
    LONGLONG    LastObservationHns;
    volatile LONG Completed;
    volatile LONG Slow;
    volatile LONG Failed;
    volatile LONG Pressure;
    ULONG       BacklogPeakUs;
    ULONG       FloorBps;
    ULONG       Downgrades;
    ULONG       Upgrades;
} PN_ABR;

typedef struct _PN_PUMP {
    PPNA2DP_DEVICE      Device;
    PN_PCM_RING         Ring;
    KSPIN_LOCK          Lock;

    /* thread control */
    HANDLE              ThreadHandle;
    PKTHREAD            Thread;
    KEVENT              WakeEvent;
    KEVENT              Idle;              /* signalled while the thread is outside a step batch */
    LONG                Running;           /* thread alive */
    LONG                StreamActive;      /* encoding/sending enabled */
    LONG                Generation;        /* increments per OPEN/CLOSE; tags host work */

    /* live changes requested by other threads, applied by the pump thread between steps */
    LONG                PendingBitpool;    /* 0 = none */
    LONG                PendingAbrReset;

    /* encoder */
    PN_ENCODER_OWNER    Owner;
    A2DP_CODEC_CONFIG   Cfg;
    sbc_encoder         Sbc;
    ULONG               PcmFramesPerCodecFrame;
    ULONG               CodecFrameBytes;      /* encoded bytes per frame (SBC) */
    ULONG               SbcBitpoolCeiling;    /* negotiated maximum, limited by the transport MTU */
    ULONG               LdacBaseQuality;     /* configured ceiling; ABR only reduces from it */
    ULONG               CodecFramesPerPacket;
    ULONG               EncoderPcmFormat;     /* PNA2DP_FMT_* expected by the encoder */
    PUCHAR              ConvBuffer;           /* PCM converted to encoder format */
    ULONG               ConvCapacity;

    /* transport */
    PPN_L2CAP_CHANNEL   Media;
    ULONG               MediaMtu;
    RTP_STATE           Rtp;
    BOOLEAN             UseRtpHeader;
    BOOLEAN             UsePayloadHeader;     /* 1-byte frame count header */
    ULONG               TargetLatencyMs;
    ULONG               MaxLatencyMs;
    ULONG               MaxWritesInFlight;

    /* host path */
    ULONG               HostNextSequence;
    LONG                HostOutstanding;
    LIST_ENTRY          HostResults;          /* PN_ENCODED_BLOCK ordered by sequence */
    ULONG               HostResultCount;      /* under HostLock */
    KSPIN_LOCK          HostLock;
    ULONG               HostTimeouts;
    ULONG               HostRoundTripAvgUs;
    LONGLONG            HostLastSubmitHns;

    /* volume (software path) */
    LONG                VolumeDb16;           /* 16.16 dB, 0 = unity */
    ULONG               VolumeGainQ15;        /* linear gain, Q15 (32768 = unity) */
    BOOLEAN             Muted;

    PN_ABR              Abr;
    PNA2DP_QUALITY      Quality;        /* published snapshot, protected by Lock */
    PNA2DP_STATS        Stats;
    ULONG               TxAvgUs;
    ULONG               TxMaxUs;
    volatile ULONG      SampleState;    /* 0 empty, 1 armed, 2 ready; Lock protects sample */
    PNA2DP_SBC_SAMPLE    Sample;
} PN_PUMP, *PPN_PUMP;

NTSTATUS PumpInitialize(_Out_ PPN_PUMP P, _In_ PPNA2DP_DEVICE Device);
VOID     PumpUninitialize(_Inout_ PPN_PUMP P);

/* Called when the WaveRT stream is created with the engine's PCM format. */
NTSTATUS PumpConfigureInput(_Inout_ PPN_PUMP P, _In_ ULONG SampleRate, _In_ ULONG Channels,
                            _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat);
VOID     PumpReleaseInput(_Inout_ PPN_PUMP P);

/* DISPATCH: copy bytes from the WaveRT buffer into the ring. */
VOID     PumpWritePcm(_Inout_ PPN_PUMP P, _In_reads_bytes_(Length) const UCHAR *Data, _In_ ULONG Length);
VOID     PumpFlush(_Inout_ PPN_PUMP P);

/* Start/stop encoding and transmission on a connected media channel (PASSIVE). */
NTSTATUS PumpStartStream(_Inout_ PPN_PUMP P, _In_ const A2DP_CODEC_CONFIG *Cfg, _In_ PPN_L2CAP_CHANNEL Media,
                         _In_ ULONG MediaMtu, _In_ PN_ENCODER_OWNER Owner, _In_ const PNA2DP_CONFIG *User);
VOID     PumpStopStream(_Inout_ PPN_PUMP P);
BOOLEAN  PumpIsStreaming(_In_ PPN_PUMP P);

/* Live parameter changes */
VOID     PumpSetVolume(_Inout_ PPN_PUMP P, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted);
VOID     PumpSetSbcBitpool(_Inout_ PPN_PUMP P, _In_ UCHAR Bitpool);
VOID     PumpSetAbr(_Inout_ PPN_PUMP P, _In_ BOOLEAN Enable);

/* Transport feedback (DISPATCH) */
VOID     PumpOnWriteDone(_Inout_ PPN_PUMP P, _In_ NTSTATUS Status, _In_ ULONG Bytes, _In_ LONGLONG SubmitTimeHns);

/* Codec host results (PASSIVE, from IOCTL) */
NTSTATUS PumpHostResult(_Inout_ PPN_PUMP P, _In_ const PNA2DP_CODEC_RESULT *R, _In_reads_bytes_(R->PayloadBytes) const UCHAR *Payload);
VOID     PumpHostLost(_Inout_ PPN_PUMP P);

VOID     PumpGetStats(_In_ PPN_PUMP P, _Out_ PPNA2DP_STATS Stats);
VOID     PumpGetQuality(_In_ PPN_PUMP P, _Out_ PNA2DP_QUALITY *Quality);
VOID     PumpResetStats(_Inout_ PPN_PUMP P);
NTSTATUS PumpSbcSample(_Inout_ PPN_PUMP P, _In_ ULONG Action, _Out_opt_ PNA2DP_SBC_SAMPLE *Sample);
ULONG    PumpCurrentBitrate(_In_ PPN_PUMP P);
ULONG    PumpEstimatedLatencyUs(_In_ PPN_PUMP P);
