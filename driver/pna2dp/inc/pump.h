/*
 * pump.h - the streaming engine.
 *
 *   WaveRT timer (DISPATCH)  --PCM bytes-->  PCM ring  --> pump thread (PASSIVE, high prio)
 *                                                         |-> kernel SBC encoder
 *                                                         |-> user-mode codec host (inverted call)
 *                                                         --> RTP packetiser --> L2CAP media channel
 *
 * The WaveRT side is the clock. The pump drains whatever PCM is available, converts it to
 * the encoder's input format and, when the engine rate is not the codec rate, to the codec
 * rate (pcm.h: a 2:1 decimation FIR and / or a polyphase stage, 44.1 / 48 / 88.2 / 96 kHz to
 * 44.1 / 48 kHz; ring, trims and PcmFramesConsumed stay in engine frames, RTP time in codec
 * frames), encodes, and keeps a bounded number of L2CAP writes in flight. A latency guard drops PCM (with a
 * short crossfade) when the ring exceeds MaxLatencyMs; the adaptive bitrate controller
 * (ABR) watches each write's own send time, the media time the link delivers and the TX
 * queue (audio_policy.h), and with a larger latency target (the Music profile,
 * config_policy.h) lets the ring absorb a hiccup before it lowers the bitrate.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "audio_policy.h"
#include "config_policy.h"
#include "pcm.h"

#define PN_PUMP_RING_MS            600     /* PCM ring capacity */
#define PN_PUMP_MAX_PACKET         2048    /* staging buffer per RTP packet */
#define PN_PUMP_MAX_PCM_BLOCK      32768   /* ring bytes handed to an encoder at once (15 SBC frames from 96 kHz S32 at 2:1; 14 to 44.1 kHz) */
#define PN_PUMP_HOST_WINDOW        4       /* outstanding encode blocks at the host (<= PN_HOST_LEDGER_SLOTS) */
#define PN_PUMP_HOST_TIMEOUT_MS    200     /* an unanswered request is written off after this */
#define PN_PUMP_HOST_BLOCKS        (2 * PN_PUMP_HOST_WINDOW)   /* preallocated result blocks */
#define PN_PUMP_DEFAULT_TARGET_MS  PN_LATENCY_BALANCED_TARGET_MS   /* the Balanced profile (config_policy.h) */
#define PN_PUMP_DEFAULT_MAX_MS     PN_LATENCY_BALANCED_MAX_MS
#define PN_PUMP_TICK_MS            4       /* safety tick while streaming (latency guard, host watchdog); work
                                              * is event driven. bthlink frees the write slot before it reports
                                              * the completion (T14), so this could grow; kept until heard on hardware */
#define PN_PUMP_FADE_BYTES         8192    /* outgoing side of a trim crossfade (4 ms at 192 kHz stereo S32 = 6144) */
#define PN_PUMP_RATE_CONVERT       PN_RATECONV_ANY   /* engine rates PumpStartStream bridges (C8, 0.1.0.22 #1; PnPcmRatePlan) */

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
    ULONG       SkipPcmFrames;     /* RTP time of dropped blocks before this one */
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
    volatile LONG QueueFull;       /* steps that found the TX queue full */
    ULONG       Trims;             /* latency trims since the last observation (pump thread) */
    ULONG       BacklogPeakUs;     /* ring latency while the TX queue was full */
    ULONG       LastRingUs;        /* ring latency at the previous observation (draining, PnAbrReasons) */
    ULONG       FloorBps;
    ULONG       Downgrades;
    ULONG       Upgrades;
    ULONG       ChangeSequence;
    ULONG       LastChangeReason;
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
    LONG                LatencyEpoch;      /* increments after each start / stop flushed the ring (PumpLatencyEpoch) */

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
    ULONG               SbcSduBytes;          /* baseband SDU SBC packets are sized to (A2DP_SBC_STREAM_PLAN) */
    ULONG               PayloadRoom;          /* media MTU less the RTP and payload headers */
    ULONG               EncoderPcmFormat;     /* PNA2DP_FMT_* expected by the encoder */
    PN_PCM_RATE_PLAN    Rate;                 /* engine rate -> codec rate (pcm.h); read at DISPATCH while StreamActive */
    PUCHAR              ConvBuffer;           /* PCM converted to encoder format */
    ULONG               ConvCapacity;

    /* transport */
    PPN_L2CAP_CHANNEL   Media;
    ULONG               MediaMtu;
    RTP_STATE           Rtp;
    BOOLEAN             UseRtpHeader;
    BOOLEAN             UsePayloadHeader;     /* 1-byte frame count header */
    ULONG               TargetLatencyMs;
    ULONG               MaxLatencyMs;         /* as configured; clamped below the ring size where used */
    ULONG               MaxWritesInFlight;
    ULONG               TxHeaderBytes;        /* RTP + media payload header per write */
    KSPIN_LOCK          TxLock;
    PN_TX_LEDGER        TxLedger;             /* outstanding media writes, under TxLock */
    LONGLONG            TxStreamStartHns;     /* completions of writes submitted earlier are stale */
    ULONG               TxSendAvgUs;          /* smoothed per-write send time, under TxLock */
    volatile ULONG      WakeBytes;            /* one encoder input block: PCM writes wake the thread at this level */
    volatile LONG       ResumeFade;           /* PumpFinishStream faded the stream out (PumpTakeResumeFade) */
    BOOLEAN             Finished;             /* under Ring.Lock: PumpFinishStream padded, nothing written since */

    /* host path */
    ULONG               HostNextSequence;
    LONG                HostOutstanding;      /* HostLedger.Count, readable without the lock */
    PN_HOST_LEDGER      HostLedger;           /* outstanding ENCODE requests, under HostLock */
    ULONG               HostPendingSkip;      /* PCM frames of dropped blocks not yet queued, under HostLock */
    LIST_ENTRY          HostResults;          /* PN_ENCODED_BLOCK ordered by sequence */
    ULONG               HostResultCount;      /* under HostLock */
    ULONG               HostQueuedPcmFrames;  /* PCM frames of HostResults, under HostLock */
    volatile LONG       HostWaitingForWork;   /* a block waits for the host to pend GET_WORK */
    LIST_ENTRY          HostFreeBlocks;       /* preallocated PN_ENCODED_BLOCKs, under HostLock */
    KSPIN_LOCK          HostLock;
    ULONG               HostTimeouts;
    PN_HOST_STALL       HostStall;            /* one failure per stall episode (finding #19), under HostLock */
    ULONG               HostRoundTripAvgUs;
    LONGLONG            HostLastSubmitHns;

    /* volume (software path) */
    LONG                VolumeDb16;           /* 16.16 dB, 0 = unity */
    ULONG               VolumeGainQ15;        /* linear gain, Q15 (32768 = unity) */
    BOOLEAN             Muted;
    PN_PCM_STATE        PcmState;             /* gain ramp + trim crossfade, pump thread only */
    PUCHAR              FadeBuffer;           /* PN_PUMP_FADE_BYTES */

    PN_ABR              Abr;
    BOOLEAN             StallReported;  /* C4: this stall episode was reported to the device (pump thread) */
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
/* <= DISPATCH, when the engine stops feeding the ring (KS pause, its end of stream; findings
 * #15/#26): Tail (the WaveRT stream's held-back frames, may be NULL) is appended and, while a
 * stream runs, the PCM waiting for a whole encoder block ends with a short fade and is padded
 * with silence, plus enough silence to drain the encoder's delay, so the receiver hears the
 * sound end instead of an underrun mid-waveform and a later start carries no stale fragment.
 * All under one ring-lock hold: the pump thread cannot encode the tail unfaded in between
 * (findings #1/#7). An empty ring is padded too; once per run of written audio. */
VOID     PumpFinishStream(_Inout_ PPN_PUMP P, _In_reads_bytes_opt_(TailBytes) const UCHAR *Tail, _In_ ULONG TailBytes);
/* <= DISPATCH, at the next KS run: TRUE once after PumpFinishStream faded the stream out, so
 * the resumed audio fades in from that silence. */
BOOLEAN  PumpTakeResumeFade(_Inout_ PPN_PUMP P);

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
/* <= DISPATCH: the host pended a GET_WORK request; wakes the pump if a block waits for one. */
VOID     PumpHostWorkPended(_Inout_ PPN_PUMP P);

VOID     PumpGetStats(_In_ PPN_PUMP P, _Out_ PPNA2DP_STATS Stats);
VOID     PumpGetQuality(_In_ PPN_PUMP P, _Out_ PNA2DP_QUALITY *Quality);
VOID     PumpResetStats(_Inout_ PPN_PUMP P);
NTSTATUS PumpSbcSample(_Inout_ PPN_PUMP P, _In_ ULONG Action, _Out_opt_ PNA2DP_SBC_SAMPLE *Sample);
ULONG    PumpCurrentBitrate(_In_ PPN_PUMP P);
/* <= DISPATCH: the stream's effective bitrate ceiling (ABR level 0 after the MTU caps), 0 when not streaming. */
ULONG    PumpCeilingBitrate(_In_ PPN_PUMP P);
/* <= DISPATCH: the WaveRT side found no new data for `Packets` packets (contract C5). */
VOID     PumpNoteWaveStarvation(_Inout_ PPN_PUMP P, _In_ ULONG Packets);
/* <= DISPATCH (contract C3): PCM in the ring + held by the codec host + encoded and
 * queued + in L2CAP writes, plus the measured send time of one write. Sink delay excluded. */
ULONG    PumpPipelineLatencyUs(_In_ PPN_PUMP P);
ULONG    PumpEstimatedLatencyUs(_In_ PPN_PUMP P);   /* = PumpPipelineLatencyUs */
ULONG    PumpLatencyEpoch(_In_ PPN_PUMP P);         /* changes whenever a stream starts or stops (ring flushed) */
/* <= DISPATCH (contract C4): streaming, media writes outstanding and none completed for ThresholdMs. */
BOOLEAN  PumpMediaStalled(_In_ PPN_PUMP P, _In_ ULONG ThresholdMs);
