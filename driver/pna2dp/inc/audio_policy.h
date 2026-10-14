/* Pure endpoint/transport decisions shared with regression tests.
 * SPDX-License-Identifier: MIT */
#pragma once

/* ---- Sample-rate conversion in the pump (0.1.0.22 #1, pcm.h PnPcmRatePlan) -----------------
 * What the pump bridges between the Windows engine (WaveRT) rate and the codec rate. */
#define PN_RATECONV_NONE   0u   /* equal rates only */
#define PN_RATECONV_HALF   1u   /* C8: also exactly 2:1, 96 -> 48 and 88.2 -> 44.1 kHz (0.1.0.21) */
#define PN_RATECONV_ANY    2u   /* #1: also 44.1 <-> 48 kHz and 88.2 / 96 kHz to the other family */

/* TRUE when Level bridges an engine at Engine Hz to a codec at Codec Hz. At PN_RATECONV_ANY it
 * is exactly the set PnPcmRatePlan converts (core_test checks every pair of A2DP rates). */
static inline BOOLEAN PnRateConvertible(ULONG Engine, ULONG Codec, ULONG Level)
{
    if (!Engine || !Codec) return FALSE;
    if (Engine == Codec) return TRUE;
    if (Level >= PN_RATECONV_HALF && ((Engine == 96000 && Codec == 48000) || (Engine == 88200 && Codec == 44100))) return TRUE;
    return Level >= PN_RATECONV_ANY && (Engine == 44100 || Engine == 48000 || Engine == 88200 || Engine == 96000) &&
           (Codec == 44100 || Codec == 48000);
}

/* Stream format reconciliation must leave in-band bitrate adaptation alone. */
static inline BOOLEAN PnStreamFormatNeedsUpdate(ULONG Codec, ULONG CurrentRate,
    ULONG CurrentMode, ULONG SelectedRate, ULONG SelectedMode)
{
    return CurrentRate != SelectedRate ||
        (Codec == PNA2DP_CODEC_SBC && CurrentMode != SelectedMode);
}

static inline BOOLEAN PnEndpointIsHeadphones(ULONG ClassOfDevice)
{
    ULONG major = (ClassOfDevice >> 8) & 0x1f;
    ULONG minor = (ClassOfDevice >> 2) & 0x3f;
    /* Bluetooth Audio/Video: wearable headset, hands-free, headphones.
     * Loudspeakers, receivers and unspecified sinks use speaker presentation. */
    return major == 4 && (minor == 1 || minor == 2 || minor == 6);
}

static inline BOOLEAN PnMediaQueueHasCapacity(ULONG InFlight, ULONG Required, ULONG Limit)
{
    return Required && InFlight < Limit && Required <= Limit - InFlight;
}

static inline ULONG PnLdacAbrQuality(ULONG ConfiguredQuality, ULONG Reductions)
{
    ULONG base = ConfiguredQuality <= 2 ? ConfiguredQuality : 1;
    return Reductions >= 2 - base ? 2 : base + Reductions;
}

/* Classic EDR capability is relevant to A2DP; LE/version/brand is not a
 * throughput measurement. Unknown feature data must not imply a weak radio. */
static inline BOOLEAN PnAdapterNeedsConservativeProfile(BOOLEAN Valid, ULONGLONG Features)
{
    return Valid && Features && !(Features & ((1ull << 25) | (1ull << 26)));
}

typedef struct _PN_QUALITY_WINDOW {
    ULONG BadMs;
    ULONG GoodMs;
    ULONG SinceChangeMs;
    ULONG RecoverySteps;
    ULONG RecoveryPenaltyMs;
    BOOLEAN LastChangeUp;
    BOOLEAN UrgentSpent;    /* this congestion episode had its urgent step (PnQualityObserveEx) */
} PN_QUALITY_WINDOW;

static inline ULONG PnQualityRecoveryTarget(const PN_QUALITY_WINDOW *S)
{
    if (S->RecoveryPenaltyMs) return S->RecoveryPenaltyMs;
    return S->RecoverySteps == 0 ? 30000 : S->RecoverySteps == 1 ? 10000 : 5000;
}

/* Return +1 to reduce bitrate, -1 to recover, 0 to keep it. Only continuously
 * observed healthy traffic earns recovery time; idle and long scheduling gaps
 * cannot masquerade as a stable link. The caller owns this state on one thread. */
static inline int PnQualityObserve(PN_QUALITY_WINDOW *S, ULONG ElapsedMs,
    BOOLEAN Traffic, BOOLEAN Congested, BOOLEAN Enable, ULONG Level, ULONG MaxLevel)
{
    if (ElapsedMs > 1000) { S->GoodMs = S->BadMs = S->RecoverySteps = 0; ElapsedMs = 250; }
    S->SinceChangeMs = S->SinceChangeMs >= 60000 ? 60000 : S->SinceChangeMs + ElapsedMs;
    if (Congested) {
        S->GoodMs = S->RecoverySteps = 0;
        S->BadMs = S->BadMs >= 1000 ? 1000 : S->BadMs + ElapsedMs;
    } else {
        S->BadMs = 0;
        S->GoodMs = Traffic ? (S->GoodMs >= 60000 ? 60000 : S->GoodMs + ElapsedMs) : 0;
        if (!Traffic || !Enable) S->RecoverySteps = 0;
    }
    if (Enable && S->BadMs >= 1000 && S->SinceChangeMs >= 2000 && Level < MaxLevel) {
        if (S->LastChangeUp && S->SinceChangeMs <= 10000) S->RecoveryPenaltyMs = 60000;
        S->LastChangeUp = FALSE;
        S->BadMs = S->SinceChangeMs = 0;
        return 1;
    }
    ULONG target = PnQualityRecoveryTarget(S);
    if (Enable && Level && S->GoodMs >= target && S->SinceChangeMs >= target) {
        S->RecoverySteps = S->RecoverySteps >= 2 ? 2 : S->RecoverySteps + 1;
        S->RecoveryPenaltyMs = 0;
        S->LastChangeUp = TRUE;
        S->GoodMs = S->SinceChangeMs = 0;
        return -1;
    }
    return 0;
}

static inline ULONG PnQualityBoundBitrate(ULONG Requested, ULONG Floor, ULONG Ceiling)
{
    ULONG value = Requested < Floor ? Floor : Requested;
    return value > Ceiling ? Ceiling : value;
}

/* ---- Codec host health -----------------------------------------------------
 * One bad result or one lost request costs one block of audio, not the codec. Only a
 * run of consecutive failures quarantines the host; every quarantine expires on its
 * own (30 s, then 2 min, then 10 min) and sustained success forgives the escalation.
 * Times are monotonic milliseconds. The caller serialises access (codec host lock). */
#define PN_HOST_FAILURE_LIMIT        4u        /* consecutive failures that quarantine */
#define PN_HOST_HEALTHY_RESET_MS     300000u   /* continuous success that resets escalation */
#define PN_HOST_HEALTHY_GAP_MS       2000u     /* a longer pause restarts the success run */

typedef struct _PN_HOST_HEALTH {
    ULONG     ConsecutiveFailures;
    ULONG     Quarantines;           /* escalation: 0 none yet, 1 = 30 s, 2 = 2 min, 3 = 10 min */
    ULONGLONG QuarantineUntilMs;     /* 0 = not quarantined */
    ULONGLONG HealthySinceMs;        /* start of the current success run, 0 = none */
    ULONGLONG LastSuccessMs;
} PN_HOST_HEALTH;

static inline ULONG PnHostQuarantineMs(ULONG Step)
{
    return Step <= 1 ? 30000u : Step == 2 ? 120000u : 600000u;
}

static inline BOOLEAN PnHostHealthQuarantined(const PN_HOST_HEALTH *S, ULONGLONG NowMs)
{
    return S->QuarantineUntilMs != 0 && NowMs < S->QuarantineUntilMs;
}

/* Returns TRUE when this failure starts a quarantine (the caller re-evaluates codecs). */
static inline BOOLEAN PnHostHealthFailure(PN_HOST_HEALTH *S, ULONGLONG NowMs)
{
    S->HealthySinceMs = 0;
    /* late failures of a stream that is already being abandoned */
    if (PnHostHealthQuarantined(S, NowMs)) return FALSE;
    if (++S->ConsecutiveFailures < PN_HOST_FAILURE_LIMIT) return FALSE;
    S->ConsecutiveFailures = 0;
    if (S->Quarantines < 3) S->Quarantines++;
    ULONGLONG until = NowMs + PnHostQuarantineMs(S->Quarantines);
    S->QuarantineUntilMs = until < NowMs ? ~0ull : until;   /* saturate instead of wrapping */
    return TRUE;
}

static inline VOID PnHostHealthSuccess(PN_HOST_HEALTH *S, ULONGLONG NowMs)
{
    S->ConsecutiveFailures = 0;
    if (PnHostHealthQuarantined(S, NowMs)) return;
    if (!S->HealthySinceMs || NowMs - S->LastSuccessMs > PN_HOST_HEALTHY_GAP_MS) S->HealthySinceMs = NowMs ? NowMs : 1;
    else if (NowMs - S->HealthySinceMs >= PN_HOST_HEALTHY_RESET_MS) S->Quarantines = 0;
    S->LastSuccessMs = NowMs;
}

/* TRUE exactly once when a quarantine has run out. */
static inline BOOLEAN PnHostHealthExpire(PN_HOST_HEALTH *S, ULONGLONG NowMs)
{
    if (!S->QuarantineUntilMs || NowMs < S->QuarantineUntilMs) return FALSE;
    S->QuarantineUntilMs = 0;
    S->ConsecutiveFailures = 0;
    S->HealthySinceMs = 0;
    return TRUE;
}

/* A (re)registered host process gets a fresh chance but keeps its escalation. */
static inline VOID PnHostHealthRegistered(PN_HOST_HEALTH *S)
{
    S->ConsecutiveFailures = 0;
    S->QuarantineUntilMs = 0;
    S->HealthySinceMs = 0;
}

/* ---- Codec host request ledger ---------------------------------------------
 * Every ENCODE request handed to the host, oldest first. The host answers in
 * submission order, so a result for a later sequence proves the earlier requests
 * were lost. PCM accounting: Submitted - Returned is the PCM the host holds. */
#define PN_HOST_LEDGER_SLOTS  8u

typedef struct _PN_HOST_LEDGER {
    ULONG     Head;                              /* oldest outstanding request */
    ULONG     Count;
    ULONG     Sequence[PN_HOST_LEDGER_SLOTS];
    ULONG     PcmFrames[PN_HOST_LEDGER_SLOTS];
    LONGLONG  SubmitHns[PN_HOST_LEDGER_SLOTS];
    ULONGLONG SubmittedFrames;                   /* PCM frames handed to the host */
    ULONGLONG ReturnedFrames;                    /* PCM frames returned by results or written off */
} PN_HOST_LEDGER;

typedef struct _PN_HOST_MATCH {
    BOOLEAN   Matched;              /* FALSE: stale, duplicate or unknown sequence; ignore it */
    ULONG     Lost;                 /* earlier requests written off by this result */
    ULONG     LostPcmFrames;
    ULONG     SubmittedPcmFrames;   /* of the matched request */
    LONGLONG  SubmitHns;            /* of the matched request */
} PN_HOST_MATCH;

static inline ULONGLONG PnHostLedgerHeldFrames(const PN_HOST_LEDGER *L)
{
    return L->SubmittedFrames - L->ReturnedFrames;
}

static inline BOOLEAN PnHostLedgerAdd(PN_HOST_LEDGER *L, ULONG Sequence, ULONG PcmFrames, LONGLONG NowHns)
{
    if (L->Count >= PN_HOST_LEDGER_SLOTS) return FALSE;
    ULONG i = (L->Head + L->Count) % PN_HOST_LEDGER_SLOTS;
    L->Sequence[i] = Sequence;
    L->PcmFrames[i] = PcmFrames;
    L->SubmitHns[i] = NowHns;
    L->Count++;
    L->SubmittedFrames += PcmFrames;
    return TRUE;
}

/* Undo the newest Add when its request could not be delivered. */
static inline VOID PnHostLedgerCancelNewest(PN_HOST_LEDGER *L, ULONG Sequence)
{
    if (!L->Count) return;
    ULONG i = (L->Head + L->Count - 1) % PN_HOST_LEDGER_SLOTS;
    if (L->Sequence[i] != Sequence) return;
    L->SubmittedFrames -= L->PcmFrames[i];
    L->Count--;
}

/* Write off the oldest outstanding request (watchdog). Returns its PCM frames. */
static inline ULONG PnHostLedgerDropOldest(PN_HOST_LEDGER *L)
{
    if (!L->Count) return 0;
    ULONG frames = L->PcmFrames[L->Head];
    L->Head = (L->Head + 1) % PN_HOST_LEDGER_SLOTS;
    L->Count--;
    L->ReturnedFrames += frames;
    return frames;
}

static inline LONGLONG PnHostLedgerOldestSubmit(const PN_HOST_LEDGER *L)
{
    return L->Count ? L->SubmitHns[L->Head] : 0;
}

static inline PN_HOST_MATCH PnHostLedgerMatch(PN_HOST_LEDGER *L, ULONG Sequence)
{
    PN_HOST_MATCH m = { 0 };
    ULONG k;
    for (k = 0; k < L->Count; k++) if (L->Sequence[(L->Head + k) % PN_HOST_LEDGER_SLOTS] == Sequence) break;
    if (k == L->Count) return m;
    for (ULONG j = 0; j < k; j++) { m.LostPcmFrames += PnHostLedgerDropOldest(L); m.Lost++; }
    m.Matched = TRUE;
    m.SubmittedPcmFrames = L->PcmFrames[L->Head];
    m.SubmitHns = L->SubmitHns[L->Head];
    /* the matched request leaves the window; its PCM stays held until returned */
    L->Head = (L->Head + 1) % PN_HOST_LEDGER_SLOTS;
    L->Count--;
    return m;
}

/* Bounds a result's PCM claim by the PCM the host actually holds (buffering codecs
 * such as LDAC return several blocks in one packet). Returns the accepted frames. */
static inline ULONG PnHostLedgerReturn(PN_HOST_LEDGER *L, ULONG ClaimedPcmFrames)
{
    ULONGLONG held = PnHostLedgerHeldFrames(L);
    ULONG accepted = (ULONGLONG)ClaimedPcmFrames > held ? (ULONG)held : ClaimedPcmFrames;
    L->ReturnedFrames += accepted;
    return accepted;
}

/* Result metadata must be consistent before it reaches the packetiser: every codec
 * frame occupies at least one byte, and the SBC/LDAC media payload header is one
 * byte carrying a 4-bit frame count. */
static inline BOOLEAN PnHostResultValid(ULONG PayloadBytes, ULONG MaxPayload, ULONG FrameCount,
    ULONG PayloadHeaderByte, BOOLEAN UsesPayloadHeader)
{
    if (PayloadBytes > MaxPayload || FrameCount > PayloadBytes) return FALSE;
    if ((PayloadBytes != 0) != (FrameCount != 0)) return FALSE;
    if (PayloadHeaderByte > 0xFF) return FALSE;
    if (UsesPayloadHeader && FrameCount > 15) return FALSE;
    if (UsesPayloadHeader && PayloadHeaderByte && (PayloadHeaderByte & 0x0F) != FrameCount) return FALSE;
    return TRUE;
}

/* ---- AAC packet fit ----------------------------------------------------------
 * Many sinks do not reassemble fragmented AAC. Cap the bitrate (and the encoder's
 * peak) so one access unit plus its LATM framing fits one RTP packet:
 * (payload room - LATM overhead) * 8 bits * rate / 1024 frames per AU.
 * PayloadRoom is the media MTU minus the 12-byte RTP header. 0 = no usable cap. */
#define PN_AAC_LATM_OVERHEAD   12u       /* AudioMuxElement + in-band StreamMuxConfig + PayloadLengthInfo */
#define PN_AAC_MAX_BITRATE     320000u

static inline ULONG PnAacMaxBitrateForPayload(ULONG PayloadRoom, ULONG SampleRate)
{
    if (PayloadRoom <= PN_AAC_LATM_OVERHEAD || !SampleRate) return 0;
    ULONGLONG bps = (ULONGLONG)(PayloadRoom - PN_AAC_LATM_OVERHEAD) * 8u * SampleRate / 1024u;
    return bps > PN_AAC_MAX_BITRATE ? PN_AAC_MAX_BITRATE : (ULONG)bps;
}

/* ---- Media transmit ledger ---------------------------------------------------
 * Every L2CAP media write the pump submits, oldest first, with the PCM frames it
 * carries (an AAC access unit's PCM rides on its last fragment). Writes on one channel
 * complete in submission order. A write's own send time excludes the time it queued
 * behind the previous one: done_i - max(submit_i, done_{i-1}). The caller serialises
 * access (pump TX lock). Times are 100 ns units. */
#define PN_TX_LEDGER_SLOTS  16u

typedef struct _PN_TX_LEDGER {
    ULONG     Head;
    ULONG     Count;
    ULONG     PcmFrames[PN_TX_LEDGER_SLOTS];
    LONGLONG  QueuedHns[PN_TX_LEDGER_SLOTS];
    ULONGLONG InFlightFrames;                    /* PCM frames of all outstanding writes */
    LONGLONG  LastDoneHns;
    /* since the last ABR observation */
    ULONGLONG DeliveredFrames;
    ULONGLONG DeliveredBits;                     /* media payload bits */
    ULONGLONG BusyUs;                            /* sum of per-write send times */
    ULONG     Stalls;                            /* writes left out of DeliveredBits / BusyUs */
} PN_TX_LEDGER;

/* A write whose own send time exceeds this was held by a stall of the controller or the
 * link (coexistence, page scan, a retransmission storm), not sent at the link's rate: its
 * time measures the stall, not throughput (review #6). PacketUs: the media time it carries. */
#define PN_TX_STALL_FACTOR   4u
#define PN_TX_STALL_MIN_US   20000u
static inline ULONG PnTxStallBoundUs(ULONG PacketUs)
{
    ULONGLONG b = (ULONGLONG)PacketUs * PN_TX_STALL_FACTOR;
    return b < PN_TX_STALL_MIN_US ? PN_TX_STALL_MIN_US : b > 0x7fffffffull ? 0x7fffffffu : (ULONG)b;
}

static inline BOOLEAN PnTxLedgerPush(PN_TX_LEDGER *L, ULONG PcmFrames, LONGLONG NowHns)
{
    if (L->Count >= PN_TX_LEDGER_SLOTS) return FALSE;
    ULONG i = (L->Head + L->Count) % PN_TX_LEDGER_SLOTS;
    L->PcmFrames[i] = PcmFrames;
    L->QueuedHns[i] = NowHns;
    L->Count++;
    L->InFlightFrames += PcmFrames;
    return TRUE;
}

/* The newest write was never sent (its submission failed synchronously). */
static inline VOID PnTxLedgerCancelNewest(PN_TX_LEDGER *L)
{
    if (!L->Count) return;
    L->Count--;
    L->InFlightFrames -= L->PcmFrames[(L->Head + L->Count) % PN_TX_LEDGER_SLOTS];
}

/* The oldest write completed. Returns its PCM frames; *SendUs is its own send time. A
 * write whose send time exceeds StallUs (0: no bound) counts as delivered media but not
 * toward the throughput (DeliveredBits / BusyUs): Stalls counts it. */
static inline ULONG PnTxLedgerCompleteEx(PN_TX_LEDGER *L, LONGLONG SubmitHns, LONGLONG DoneHns,
    ULONG PayloadBytes, ULONG StallUs, ULONG *SendUs)
{
    LONGLONG start = SubmitHns > L->LastDoneHns ? SubmitHns : L->LastDoneHns;
    LONGLONG send = DoneHns > start ? (DoneHns - start) / 10 : 0;
    *SendUs = send > 0x7fffffffll ? 0x7fffffffu : (ULONG)send;
    if (DoneHns > L->LastDoneHns) L->LastDoneHns = DoneHns;
    if (!L->Count) return 0;
    ULONG frames = L->PcmFrames[L->Head];
    L->Head = (L->Head + 1) % PN_TX_LEDGER_SLOTS;
    L->Count--;
    L->InFlightFrames -= frames;
    L->DeliveredFrames += frames;
    if (StallUs && *SendUs > StallUs) {
        L->Stalls++;
    } else {
        L->DeliveredBits += (ULONGLONG)PayloadBytes * 8u;
        L->BusyUs += *SendUs;
    }
    return frames;
}

static inline ULONG PnTxLedgerComplete(PN_TX_LEDGER *L, LONGLONG SubmitHns, LONGLONG DoneHns,
    ULONG PayloadBytes, ULONG *SendUs)
{
    return PnTxLedgerCompleteEx(L, SubmitHns, DoneHns, PayloadBytes, 0, SendUs);
}

/* PCM frames of the oldest outstanding write (the next to complete); 0 = none or a fragment
 * that carries none. */
static inline ULONG PnTxLedgerOldestFrames(const PN_TX_LEDGER *L)
{
    return L->Count ? L->PcmFrames[L->Head] : 0;
}

/* The media time a write carries: its PCM frames at the codec rate when it carries any (VBR
 * AAC and LDAC writes vary in size, not in media time: review round 4 #4), else estimated
 * from its size at the stream bitrate (a fragment without frames). */
static inline ULONG PnTxWriteMediaUs(ULONG PcmFrames, ULONG SampleRate, ULONG Bytes, ULONG BitrateBps)
{
    if (PcmFrames && SampleRate) return (ULONG)((ULONGLONG)PcmFrames * 1000000ull / SampleRate);
    return BitrateBps ? (ULONG)((ULONGLONG)Bytes * 8000000ull / BitrateBps) : 20000u;
}

/* Slow: the link took longer to send a write than the media it carries lasts, beyond the
 * stack's and the controller's fixed completion latency. A write that did not queue behind
 * another one measures that latency in full (2-8 ms on the Sony path, whose writes carry
 * 8 ms): without the allowance ordinary jitter walked SBC down (review round 4 #3). */
#define PN_TX_SLOW_ALLOWANCE_US   10000u
static inline BOOLEAN PnTxWriteSlow(ULONG SendUs, ULONG MediaUs)
{
    return (ULONGLONG)SendUs > (ULONGLONG)MediaUs + PN_TX_SLOW_ALLOWANCE_US;
}

/* Writes are outstanding and none has completed for ThresholdMs: the controller or the
 * receiver stopped taking media. Measured from the later of the last completion and
 * the oldest outstanding write's submission, so an idle gap before it does not count. */
static inline BOOLEAN PnTxLedgerStalled(const PN_TX_LEDGER *L, LONGLONG NowHns, ULONG ThresholdMs)
{
    if (!L->Count || !ThresholdMs) return FALSE;
    LONGLONG oldest = L->QueuedHns[L->Head];
    LONGLONG since = oldest > L->LastDoneHns ? oldest : L->LastDoneHns;
    return NowHns - since >= (LONGLONG)ThresholdMs * 10000;
}

/* C4: the pump reports a stall to the device once per episode. A receiver is declared
 * unreachable only after this long (the host's 200 ms write-off is far too short for a
 * radio link); *Reported re-arms once writes complete again or a stream starts. */
#define PN_MEDIA_STALL_MS   2000u

static inline BOOLEAN PnStallEdge(BOOLEAN Stalled, BOOLEAN *Reported)
{
    if (!Stalled) { *Reported = FALSE; return FALSE; }
    if (*Reported) return FALSE;
    *Reported = TRUE;
    return TRUE;
}

/* ---- Pump wakeups ---------------------------------------------------------------
 * A PCM write (every WaveRT timer tick) wakes the pump thread only when it completes
 * one encoder input block; 0 BlockBytes (unknown) wakes on every write. */
static inline BOOLEAN PnPcmWriteShouldWake(ULONG AvailBefore, ULONG Length, ULONG BlockBytes, BOOLEAN Overrun)
{
    return Overrun || !BlockBytes ||
        (AvailBefore < BlockBytes && (ULONGLONG)AvailBefore + Length >= BlockBytes);
}

/* ---- End of the PCM at a pause or end of stream (PumpFinishStream, findings #15/#26) -------
 * The PCM waiting for a whole encoder block is padded with silence up to whole blocks of
 * BlockBytes (the wake level: the most one block takes), plus ExtraBlocks blocks:
 *   - the codec's flush, as at equal rates: Base blocks drain what the encoder holds (its delay,
 *     encode + decode: SBC 73 codec frames (37 with 4 subbands), aptX 90, LDAC within its block;
 *     FDK AAC ~1600 frames: 2 blocks);
 *   - with a rate converter (DelayEngineFrames != 0), as many more as its delay and a short block
 *     need: its blocks take up to BlockFrames - 1 frames less than the padding's unit
 *     (PnPcmMaxInputFrames), so the pump can stop up to BlockFrames - 1 short of the padding's end,
 *     and the converter holds DelayEngineFrames of what it took.
 * With Extra = Base + ceil((DelayEngineFrames + BlockFrames - 1) / BlockFrames) the pump takes at
 * least rendered + DelayEngineFrames + Base * BlockFrames: every rendered frame leaves the
 * converter and the encoder gets at least the equal-rate flush after it, at any backlog, residual
 * and converter phase (core_test TestRateConverterPauseDrain sweeps them). Fewer blocks left the
 * sound ending mid-fade, its rest coming out after the resume (0.1.0.22 #1 with one block for a
 * polyphase stage; with the alias-free filters' longer delays, the encoder's last ~0.7 ms of the
 * fade, review of 0.1.0.22 #1). BlockFrames 0 (unknown): the codec's flush only. Within RoomBytes,
 * in whole blocks. */
static inline ULONG PnFinishExtraBlocks(ULONG Codec, ULONG BlockFrames, ULONG DelayEngineFrames)
{
    ULONG base = Codec == PNA2DP_CODEC_AAC ? 2u : 1u;
    if (!BlockFrames || !DelayEngineFrames) return base;
    return base + (ULONG)(((ULONGLONG)DelayEngineFrames + 2ull * BlockFrames - 2) / BlockFrames);
}

static inline ULONG PnFinishPadBytes(ULONG AvailBytes, ULONG BlockBytes, ULONG ExtraBlocks, ULONG RoomBytes)
{
    if (!BlockBytes) return 0;
    ULONG residual = AvailBytes % BlockBytes;
    ULONGLONG pad = (ULONGLONG)(residual ? BlockBytes - residual : 0) + (ULONGLONG)ExtraBlocks * BlockBytes;
    if (pad > RoomBytes) pad = RoomBytes - RoomBytes % BlockBytes;
    return (ULONG)pad;
}

/* ---- Pipeline latency (contract C3) --------------------------------------------
 * PCM still in the ring (at the engine rate) plus PCM held further down the pipeline
 * (at the codec rate: at the codec host, in encoded results, in L2CAP writes) plus the
 * measured send time of one write. The sink's own delay is not included. */
static inline ULONG PnPipelineLatencyUs(ULONGLONG RingFrames, ULONG RingRate, ULONGLONG CodecFrames,
    ULONG CodecRate, ULONG SendUs)
{
    ULONGLONG us = SendUs;
    if (RingRate) us += RingFrames * 1000000ull / RingRate;
    if (CodecRate) us += CodecFrames * 1000000ull / CodecRate;
    return us > 0xffffffffull ? 0xffffffffu : (ULONG)us;
}

/* ---- Latency guard ------------------------------------------------------------
 * MaxLatencyMs may be configured above what the ring holds; then the guard would never
 * fire and ring overruns would discard audio every tick instead. The effective ceiling
 * stays PN_RING_HEADROOM_MS below the ring capacity, and the trim target at least
 * 20 ms below the ceiling. */
#define PN_RING_HEADROOM_MS   100u
#define PN_TRIM_FADE_MS       4u          /* crossfade across a latency trim */

static inline VOID PnEffectiveLatencyMs(ULONG TargetMs, ULONG MaxMs, ULONG RingMs, ULONG *EffTargetMs, ULONG *EffMaxMs)
{
    ULONG cap = RingMs > PN_RING_HEADROOM_MS + 40 ? RingMs - PN_RING_HEADROOM_MS : 40;
    ULONG max = MaxMs < 40 ? 40 : MaxMs > cap ? cap : MaxMs;
    *EffMaxMs = max;
    *EffTargetMs = TargetMs > max - 20 ? max - 20 : TargetMs;
}

/* A trim keeps KeepBytes of the newest PCM. Returns the bytes to discard (frame
 * aligned) and in *FadeFrames how many frames at the cut form each side of the
 * crossfade: the discarded audio that would have played next, and the kept audio. */
static inline ULONG PnTrimPlan(ULONG AvailBytes, ULONG KeepBytes, ULONG BytesPerFrame,
    ULONG FadeFramesWanted, ULONG FadeCapBytes, ULONG *FadeFrames)
{
    *FadeFrames = 0;
    if (!BytesPerFrame || AvailBytes <= KeepBytes) return 0;
    ULONG drop = (AvailBytes - KeepBytes) / BytesPerFrame * BytesPerFrame;
    ULONG fade = FadeFramesWanted;
    if (fade > drop / BytesPerFrame) fade = drop / BytesPerFrame;
    if (fade > (AvailBytes - drop) / BytesPerFrame) fade = (AvailBytes - drop) / BytesPerFrame;
    if (fade > FadeCapBytes / BytesPerFrame) fade = FadeCapBytes / BytesPerFrame;
    *FadeFrames = fade;
    return drop;
}

/* ---- ABR signals ------------------------------------------------------------------
 * One observation window of the media link. Only the link counts as congestion: the
 * backlog matters only while the TX queue was full, and a ring that grows while the
 * TX queue has room (codec host starvation) is no reason to lower quality. */
typedef struct _PN_ABR_SAMPLE {
    ULONG     ElapsedMs;
    ULONG     Completed;        /* writes completed */
    ULONG     Slow;             /* writes whose own send time exceeded their media time */
    ULONG     Failed;
    ULONG     Pressure;         /* packets dropped because the TX queue was full */
    ULONG     QueueFull;        /* steps that found the TX queue full */
    ULONG     Trims;            /* latency-guard trims */
    ULONG     BacklogPeakUs;    /* ring latency observed while the TX queue was full */
    ULONG     TargetLatencyUs;
    ULONGLONG DeliveredMediaUs; /* media time carried by the completed writes */
    ULONGLONG DeliveredBits;
    ULONGLONG BusyUs;
    ULONG     Stalls;           /* completed writes held by a stall: not in DeliveredBits / BusyUs */
    /* latency profiles (0.1.0.22): the buffer the profile keeps absorbs a hiccup */
    ULONG     SoftGateUs;       /* PnAbrSoftGateUs of the effective target; 0 = the 0.1.0.21 rules */
    ULONG     RingStartUs;      /* ring latency at the start and the end of the window */
    ULONG     RingEndUs;
} PN_ABR_SAMPLE;

/* A backlog shrinking at least this much within one window is draining: the link has spare
 * capacity again and the backlog is the remainder of a hiccup, not congestion. */
#define PN_ABR_DRAIN_US 10000u

/* The backlog (ring, while the TX queue is full) a SLOW reason needs before it counts with a
 * larger buffer. 0 = always: a target of 60 ms or less (Balanced, Low latency) keeps the
 * 0.1.0.21 rules exactly. Music (150 ms): 90 ms. */
static inline ULONG PnAbrSoftGateUs(ULONG EffTargetMs)
{
    return EffTargetMs > 60 ? (EffTargetMs - 60) * 1000u : 0;
}

/* PNA2DP_QUALITY_REASON_* bits for the link congestion seen in S; 0 = healthy. With a soft
 * gate (a buffer above 60 ms) the buffer is allowed to do its job: slow writes and a link
 * below real time count only once the backlog passes the gate, and a backlog the link is
 * draining (no trim in the window) is no reason at all. Failures and drops always count. */
static inline ULONG PnAbrReasons(const PN_ABR_SAMPLE *S)
{
    ULONG r = 0;
    if (S->Failed) r |= PNA2DP_QUALITY_REASON_FAILED;
    if (S->Pressure) r |= PNA2DP_QUALITY_REASON_PRESSURE;
    BOOLEAN tolerant = S->SoftGateUs != 0;
    BOOLEAN draining = tolerant && !S->Trims && (ULONGLONG)S->RingEndUs + PN_ABR_DRAIN_US <= S->RingStartUs;
    if (S->QueueFull && S->BacklogPeakUs > S->TargetLatencyUs && !draining) r |= PNA2DP_QUALITY_REASON_BACKLOG;
    BOOLEAN soft = !tolerant || (S->QueueFull && S->BacklogPeakUs > S->SoftGateUs && !draining);
    if (soft && S->Slow >= 2 && (ULONGLONG)S->Slow * 10 >= S->Completed) r |= PNA2DP_QUALITY_REASON_SLOW;
    /* the queue was full and the link delivered less than 90% of real time */
    if (soft && S->QueueFull && S->ElapsedMs >= 200 && S->DeliveredMediaUs * 10 < (ULONGLONG)S->ElapsedMs * 9000) r |= PNA2DP_QUALITY_REASON_SLOW;
    return r;
}

/* ~0.8x the payload throughput the link sustained while busy; 0 = not measurable. Writes
 * held by a stall are not part of it (review #6); at least 4 measured writes. */
static inline ULONG PnAbrBudgetBps(const PN_ABR_SAMPLE *S)
{
    if (S->Completed < S->Stalls + 4 || S->BusyUs < 50000) return 0;
    ULONGLONG bps = S->DeliveredBits * 1000000ull / S->BusyUs * 4 / 5;
    return bps > 0xffffffffull ? 0xffffffffu : (ULONG)bps;
}

/* The first rung below Level whose bitrate fits BudgetBps (Ladder[0..MaxLevel] does not
 * increase). Unknown budget, or a current rate that already fits: one rung. */
static inline ULONG PnAbrLevelForBudget(const ULONG *Ladder, ULONG MaxLevel, ULONG Level, ULONG BudgetBps)
{
    if (Level >= MaxLevel) return MaxLevel;
    if (!BudgetBps || Ladder[Level] <= BudgetBps) return Level + 1;
    for (ULONG l = Level + 1; l <= MaxLevel; l++) if (Ladder[l] <= BudgetBps) return l;
    return MaxLevel;
}

/* PnQualityObserve, plus: a latency trim caused by the link (Urgent) reduces at once,
 * without the 1 s confirmation and the 2 s cooldown, since audio was already lost; and
 * a reduction jumps to TargetLevel instead of one rung. Recovery is unchanged
 * (30/10/5 s). Once per congestion episode: a total stall trims every ~190 ms and would
 * otherwise walk the ladder down a rung per window; further reductions in the episode go
 * through the normal confirmation. Clean re-arms it (a window that delivered and never
 * found the TX queue full; review round 3 #4). Returns the rungs to reduce (> 0), -1 to
 * recover, 0 to keep. */
static inline int PnQualityObserveEx(PN_QUALITY_WINDOW *S, ULONG ElapsedMs, BOOLEAN Traffic, BOOLEAN Congested,
    BOOLEAN Urgent, BOOLEAN Clean, BOOLEAN Enable, ULONG Level, ULONG MaxLevel, ULONG TargetLevel)
{
    if (Clean) S->UrgentSpent = FALSE;
    int change = PnQualityObserve(S, ElapsedMs, Traffic, Congested || Urgent, Enable, Level, MaxLevel);
    if (change == 0 && Urgent && !S->UrgentSpent && Enable && Level < MaxLevel) {
        if (S->LastChangeUp && S->SinceChangeMs <= 10000) S->RecoveryPenaltyMs = 60000;
        S->LastChangeUp = FALSE;
        S->BadMs = S->SinceChangeMs = 0;
        S->UrgentSpent = TRUE;
        change = 1;
    }
    if (change > 0) {
        ULONG target = TargetLevel > MaxLevel ? MaxLevel : TargetLevel;
        if (target > Level + 1) change = (int)(target - Level);
    }
    return change;
}

/* One ABR decision for window S. Ladder[0..MaxLevel] holds every rung's bitrate (NULL:
 * single rungs). *Reasons receives the congestion reasons. Returns as PnQualityObserveEx. */
static inline int PnAbrDecide(PN_QUALITY_WINDOW *W, const PN_ABR_SAMPLE *S, const ULONG *Ladder,
    BOOLEAN Enable, ULONG Level, ULONG MaxLevel, ULONG *Reasons)
{
    ULONG reasons = PnAbrReasons(S);
    /* a trim is the link's fault only when the TX queue was the bottleneck */
    BOOLEAN urgent = S->Trims && (S->QueueFull || reasons);
    if (urgent) reasons |= PNA2DP_QUALITY_REASON_BACKLOG;
    *Reasons = reasons;
    ULONG target = Level + 1;
    /* the budget measures the link only while the TX queue was full; otherwise BusyUs is the
     * completion latency of the offered load (review round 4 #3/#4): one rung */
    if (reasons && Ladder && S->QueueFull) target = PnAbrLevelForBudget(Ladder, MaxLevel, Level, PnAbrBudgetBps(S));
    BOOLEAN clean = !urgent && !reasons && !S->QueueFull && S->Completed > S->Stalls;
    return PnQualityObserveEx(W, S->ElapsedMs, S->Completed != 0, reasons != 0, urgent, clean, Enable, Level, MaxLevel, target);
}


/* ---- SBC packets against Bluetooth baseband packets (P9) ----------------------
 * An L2CAP SDU that just overflows one baseband packet costs a second packet and its
 * slots. The largest ACL packet both the EDR rate and the slot count allow carries
 * 27/183/339 (DH1/3/5), 54/367/679 (2-DH1/3/5) or 83/552/1021 (3-DH1/3/5) bytes, of which
 * the L2CAP header takes 4. Feature bits are the local LMP features (bthdef.h): 0/1 3/5-slot,
 * 25/26 EDR 2/3 Mbps, 39/40 3/5-slot EDR. 0 = unknown: pack to the MTU only, as before. */
#define PN_L2CAP_HEADER_BYTES   4u
#define PN_SDU_2DH5             (679u - PN_L2CAP_HEADER_BYTES)
#define PN_SDU_3DH5             (1021u - PN_L2CAP_HEADER_BYTES)
/* SBC XQ: dual channel, 8 subbands, 16 blocks, loudness, at the bitpool plain dual channel
 * gets from the same receiver, MTU and radio (never lower: the fixed 38/47 points cost 11-27%
 * of the bitrate where the sink's own maximum already fits; known issue / findings #1, #7). */

static inline ULONG PnBasebandSduBytes(BOOLEAN Valid, ULONGLONG Features)
{
    if (!Valid || !Features) return 0;
    BOOLEAN edr2 = (Features & (1ull << 25)) != 0, edr3 = (Features & (1ull << 26)) != 0;
    if ((edr2 || edr3) && (Features & (1ull << 40))) return (edr3 ? 1021u : 679u) - PN_L2CAP_HEADER_BYTES;
    if ((edr2 || edr3) && (Features & (1ull << 39))) return (edr3 ? 552u : 367u) - PN_L2CAP_HEADER_BYTES;
    if (Features & (1ull << 1)) return 339u - PN_L2CAP_HEADER_BYTES;
    if (Features & 1ull) return 183u - PN_L2CAP_HEADER_BYTES;
    return 27u - PN_L2CAP_HEADER_BYTES;
}

/* Baseband slots an SDU of Bytes costs on the radio whose largest packet carries MaxSdu
 * (a PnBasebandSduBytes value): the largest packet type while the rest does not fit it,
 * then the smallest of that family that carries the rest; each packet plus its return slot.
 * 0 = MaxSdu is not one of those values. */
static inline ULONG PnSduSlots(ULONG MaxSdu, ULONG Bytes)
{
    /* packet families by their largest packet: payload bytes of the 1/3/5-slot types */
    static const ULONG fam[3][3] = { { 27u, 183u, 339u }, { 54u, 367u, 679u }, { 83u, 552u, 1021u } };
    ULONG top = MaxSdu + PN_L2CAP_HEADER_BYTES, f = 3, k = 3;
    for (ULONG i = 0; i < 3 && f == 3; i++)
        for (ULONG j = 0; j < 3; j++) if (fam[i][j] == top) { f = i; k = j; break; }
    if (f == 3) return 0;
    Bytes += PN_L2CAP_HEADER_BYTES;
    ULONG slots = (Bytes / top) * (2u * k + 2u), rest = Bytes % top;
    if (rest) {
        ULONG j = 0;
        while (fam[f][j] < rest) j++;   /* rest < top = fam[f][k]: stops at k at the latest */
        slots += 2u * j + 2u;
    }
    return slots;
}

/* The latency target SBC packing uses (PnSbcPacking, A2dpSbcPlanStream): the configured target
 * up to 80 ms, the largest value shipped before the latency profiles. A Music buffer does not
 * make 50 ms packets, of which one lost packet would cost 50 ms of audio. */
#define PN_SBC_PACKING_MAX_TARGET_MS 80u
static inline ULONG PnSbcPackingTargetMs(ULONG TargetLatencyMs)
{
    return TargetLatencyMs < PN_SBC_PACKING_MAX_TARGET_MS ? TargetLatencyMs : PN_SBC_PACKING_MAX_TARGET_MS;
}

/* SBC frames per media packet: fill the MTU, stay under ~Target/3 ms per packet and 15
 * frames; with SduBytes known (the largest baseband packet), the count up to that with the
 * fewest baseband slots per frame (ties: more frames, so fewer packets). The MTU-filling count
 * is a candidate, so no radio costs more air per frame than without the radio data; a short
 * SDU wins only where it is cheaper (on a BR or 3-slot radio it can cost more). The ABR repacks
 * with it at every bitpool (findings #6/#8). SduBytes 0 = the previous rule exactly. */
static inline ULONG PnSbcPacking(ULONG FrameBytes, ULONG FrameUs, ULONG TargetLatencyMs, ULONG PayloadRoom,
                                 ULONG HeaderBytes, ULONG SduBytes)
{
    if (!FrameBytes) FrameBytes = 1;
    ULONG fitMtu = PayloadRoom / FrameBytes;
    ULONG fitTime = (TargetLatencyMs * 1000u / 3u) / (FrameUs ? FrameUs : 1u);
    if (fitTime < 1u) fitTime = 1u;
    ULONG n = fitMtu < fitTime ? fitMtu : fitTime;
    if (n > 15u) n = 15u;
    if (!n) return 1u;
    if (SduBytes > HeaderBytes && PnSduSlots(SduBytes, HeaderBytes + FrameBytes)) {
        ULONG best = n;
        ULONGLONG bestSlots = PnSduSlots(SduBytes, HeaderBytes + n * FrameBytes);
        for (ULONG k = n - 1; k >= 1; k--) {
            ULONGLONG slots = PnSduSlots(SduBytes, HeaderBytes + k * FrameBytes);
            if (slots * best < bestSlots * k) { best = k; bestSlots = slots; }
        }
        n = best;
    }
    return n;
}

/* The SDU SBC XQ packs its frames to: the largest packet both ends support. 3-DH5 needs
 * 3 Mbps EDR on the sink too; with the sink's features unknown the 2-DH5 point (review
 * round 4 #5). Unknown adapter: 0, as PnBasebandSduBytes. XQ never lowers the bitpool for it. */
static inline ULONG PnSbcXqSduBytes(BOOLEAN LocalValid, ULONGLONG Local, BOOLEAN RemoteValid, ULONGLONG Remote)
{
    return PnBasebandSduBytes(LocalValid, Local & (RemoteValid ? Remote : ~(1ull << 26)));
}

/* SBC ABR ladder: from MaxBp down in steps of ~15% to MinBp. FitBp (0 = none), the largest
 * bitpool whose packets fit one 2-DH5 packet, is inserted as an extra rung when it lies
 * strictly between MinBp and MaxBp; the other rungs are unchanged. */
static inline ULONG PnSbcLadderBitpool(ULONG MaxBp, ULONG MinBp, ULONG FitBp, ULONG Level)
{
    ULONG ladder = MaxBp, bp = MaxBp;
    BOOLEAN pending = FitBp > MinBp && FitBp < MaxBp;
    for (ULONG i = 0; i < Level; i++) {
        ULONG step = ladder / 7 > 2u ? ladder / 7 : 2u;
        ULONG next = ladder <= MinBp + step ? MinBp : ladder - step;
        if (pending && FitBp > next) { bp = FitBp; pending = FALSE; continue; }
        if (pending && FitBp == next) pending = FALSE;
        ladder = bp = next;
    }
    return bp;
}

/* ---- aptX packets ---------------------------------------------------------------
 * libfreeaptx turns 4 stereo PCM frames into one 4-byte (aptX, aptX LL) or 6-byte (aptX HD)
 * codeword. Packets carried blocks for 8/12-byte codewords, so half the MTU went unused and the
 * packet rate doubled (findings #16, #33). The receiver keeps alignment only through the
 * encoder's parity sync, which cycles every 8 codewords: a packet of a multiple of 8 lets a
 * lost packet cost only its own audio instead of a decoder resync (finding #17). aptX / HD:
 * up to 160 blocks (13.3 ms at 48 kHz); aptX LL keeps short packets: at most 112 blocks
 * (9.3 ms at 48 kHz), its packet on an 895-byte MTU before. Below that MTU the old half-filled
 * packets were shorter (84 blocks, 7.0 ms at MTU 672, and not sync-aligned): LL now buffers up
 * to 2.3 ms more there, in fewer packets (less air time on 3 Mbps EDR, about 12 % more on a
 * 2 Mbps-only link). A shorter cap would bring back the doubled packet rate (finding #10). */
#define PN_APTX_SYNC_BLOCKS        8u
#define PN_APTX_MAX_BLOCKS         160u
#define PN_APTX_LL_MAX_BLOCKS      112u

static inline ULONG PnAptxCodewordBytes(ULONG Codec)
{
    return Codec == PNA2DP_CODEC_APTX_HD ? 6u : 4u;
}

/* 4-frame blocks per media packet for PayloadRoom bytes: a multiple of 8 when at least 8 fit. */
static inline ULONG PnAptxBlocksPerPacket(ULONG Codec, ULONG PayloadRoom)
{
    ULONG n = PayloadRoom / PnAptxCodewordBytes(Codec);
    ULONG cap = Codec == PNA2DP_CODEC_APTX_LL ? PN_APTX_LL_MAX_BLOCKS : PN_APTX_MAX_BLOCKS;
    if (n > cap) n = cap;
    if (n >= PN_APTX_SYNC_BLOCKS) n -= n % PN_APTX_SYNC_BLOCKS;
    return n ? n : 1u;
}

/* ---- Codec host stalls ---------------------------------------------------------
 * One stall of the user-mode host writes off its whole request window at once (4 blocks: as
 * many as PN_HOST_FAILURE_LIMIT), so counting each written-off block quarantined the host and
 * renegotiated the stream on a single 220-290 ms hiccup (finding #19). A stall episode counts
 * one failure, plus one per PN_HOST_STALL_EPISODE_MS it lasts, until a valid result ends it:
 * a host that stopped answering is still quarantined after ~2 s. Invalid results still count
 * one each. The caller serialises access (pump host lock). */
#define PN_HOST_STALL_EPISODE_MS   500u

typedef struct _PN_HOST_STALL {
    BOOLEAN   Active;
    ULONGLONG LastCountedMs;
} PN_HOST_STALL;

/* A request was written off (watchdog) or skipped (lost): TRUE when it counts as a failure. */
static inline BOOLEAN PnHostStallCounts(PN_HOST_STALL *S, ULONGLONG NowMs)
{
    if (S->Active && NowMs - S->LastCountedMs < PN_HOST_STALL_EPISODE_MS) return FALSE;
    S->Active = TRUE;
    S->LastCountedMs = NowMs;
    return TRUE;
}

static inline VOID PnHostStallEnd(PN_HOST_STALL *S)
{
    S->Active = FALSE;
}

/* ---- Software volume against the receiver's own --------------------------------
 * While Windows drives the receiver's absolute volume (AVRCP) the encoder gain is unity. When
 * that hand-off ends (the listener detaches, as on every AVDTP session restart) the receiver
 * keeps the attenuation it was last set to; applying the same level again in software played
 * V dB too quiet until absolute volume returned (findings #2, #27). The software gain is the
 * user's volume less what the receiver still applies, never above unity. dB values are 16.16. */
static inline LONG PnSoftwareVolumeDb16(BOOLEAN RemoteActive, BOOLEAN ReceiverHolds, LONG HeldDb16, LONG UserDb16)
{
    if (RemoteActive) return 0;
    if (!ReceiverHolds || HeldDb16 >= 0) return UserDb16;
    LONGLONG v = (LONGLONG)UserDb16 - HeldDb16;
    return v >= 0 ? 0 : v < -0x7fffffffll ? -0x7fffffff : (LONG)v;
}

/* ---- LDAC transport ------------------------------------------------------------
 * libldac needs a media MTU of at least 679 bytes (LDACBT_MTU_REQUIRED); below it every
 * encode fails and the host is quarantined (finding #18). */
#define PN_LDAC_MIN_MEDIA_MTU      679u

static inline BOOLEAN PnLdacMediaMtuUsable(ULONG MediaMtu)
{
    return MediaMtu >= PN_LDAC_MIN_MEDIA_MTU;
}

/* ---- Receiver-held volume and the link (findings #3/#8) --------------------------
 * When absolute volume detaches while the ACL stays up (an AVDTP session restart, the Sony
 * #2/#27 case) the receiver keeps the level it was last set to, and the software gain is the
 * user's volume less that level (PnSoftwareVolumeDb16). Once the link drops, or when the
 * listener detaches only after it dropped, the receiver's level is unknown (many reset at
 * power-on): the hold ends and the software gain is the user's volume again. That errs quiet,
 * never loud, until Windows sends absolute volume again. */
static inline VOID PnRemoteVolumeDetached(BOOLEAN *Active, BOOLEAN *Holds, LONG *HeldDb16, LONG LevelDb16, BOOLEAN AclConnected)
{
    *HeldDb16 = AclConnected ? LevelDb16 : 0;
    *Holds = AclConnected;
    *Active = FALSE;
}

static inline VOID PnRemoteVolumeLinkLost(BOOLEAN *Holds)
{
    *Holds = FALSE;
}

/* ---- Stream start fade-in (finding #4) ---------------------------------------------
 * A stream start discards the PCM waiting in the ring. The first block fades in from silence
 * only when that PCM ended in a signal: the next sample then continues a programme the
 * receiver never heard the start of. Rendered silence (the engine's zeros while the receiver
 * kept the stream suspended, the T2 resume path) ends in zeros: the next sample is a true onset
 * and stays untouched. TRUE when the newest Bytes (at most Available) of a ring of Capacity
 * bytes, written up to WritePos, hold a non-zero byte. Bounded by Bytes. */
static inline BOOLEAN PnRingTailHasSignal(const UCHAR *Buffer, ULONG Capacity, ULONGLONG WritePos, ULONG Available, ULONG Bytes)
{
    if (!Buffer || !Capacity) return FALSE;
    if (Bytes > Available) Bytes = Available;
    if (Bytes > Capacity) Bytes = Capacity;
    ULONGLONG start = WritePos - Bytes;
    for (ULONG i = 0; i < Bytes; ++i)
        if (Buffer[(ULONG)((start + i) % Capacity)]) return TRUE;
    return FALSE;
}

/* ---- KSPROPSETID_BtAudio one-shot connect / disconnect ------------------------------
 * KSPROPERTY_ONESHOT_RECONNECT and _DISCONNECT are filter properties with no value. Microsoft
 * documents them as Get only (Windows' own Connect / Disconnect send GET); SET stays accepted
 * for callers written against the earlier SET-only handler. The verbs are ks.h's
 * KSPROPERTY_TYPE_* values (audio_topo.cpp asserts they match). */
#define PN_KS_VERB_GET            0x00000001UL
#define PN_KS_VERB_SET            0x00000002UL
#define PN_KS_VERB_BASICSUPPORT   0x00000200UL
#define PN_BTAUDIO_ONESHOT_ACCESS (PN_KS_VERB_GET | PN_KS_VERB_SET | PN_KS_VERB_BASICSUPPORT)

/* TRUE when Verb asks to carry out the one-shot action (not a basic-support query). */
static inline BOOLEAN PnBtAudioOneShotVerbAccepted(ULONG Verb)
{
    if (Verb & PN_KS_VERB_BASICSUPPORT) return FALSE;
    return (Verb & (PN_KS_VERB_GET | PN_KS_VERB_SET)) ? TRUE : FALSE;
}
