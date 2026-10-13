/* Pure WaveRT stream arithmetic shared with core_test: render clock, presentation
 * position, packet starvation, timer period and topology volume rules. Integer only
 * (kernel code), callable at any IRQL; the caller provides the locking.
 * SPDX-License-Identifier: MIT */
#pragma once

#define PN_WAVE_HNS_PER_SEC         10000000ull
/* A tick later than this (timer starved for seconds) only moves 10 s worth of audio; the
 * buffer clamp drops most of it anyway. Bounds Elapsed * Rate far below 2^64 for any rate. */
#define PN_WAVE_MAX_ELAPSED_HNS     (10ull * PN_WAVE_HNS_PER_SEC)
/* Presentation latency changes are applied at no more than 1% of real time (10 ms per
 * second): fast enough to absorb a delay report within seconds, slow enough that the
 * reported clock stays within 1% of nominal for A/V sync consumers. */
#define PN_WAVE_LATENCY_SLEW_PPM    10000ull
#define PN_WAVE_MAX_LATENCY_US      10000000u   /* 10 s: bounds the 64-bit products */

/* Whole frames due for ElapsedHns more of the render clock. Remainder carries the exact
 * fractional frame in hns*Hz units (always < 10^7), so the frames handed out over any
 * number of ticks equal floor(total elapsed * Rate / 10^7): no drift. */
static inline ULONGLONG PnWaveFramesDue(ULONGLONG ElapsedHns, ULONG Rate, ULONGLONG *Remainder)
{
    if (!Rate) { *Remainder = 0; return 0; }
    if (ElapsedHns > PN_WAVE_MAX_ELAPSED_HNS) ElapsedHns = PN_WAVE_MAX_ELAPSED_HNS;
    /* <= 10^8 * 2^32 + 10^7 < 2^63 */
    ULONGLONG acc = ElapsedHns * Rate + (*Remainder % PN_WAVE_HNS_PER_SEC);
    *Remainder = acc % PN_WAVE_HNS_PER_SEC;
    return acc / PN_WAVE_HNS_PER_SEC;
}

/* Period of the render timer. Notifications fire on the first tick after a packet
 * boundary, so the period is the notification lateness. Short packets (< 5 ms) and the
 * polled (non event-driven) mode keep 1 ms; the usual 10 ms engine period gets 2 ms,
 * which halves the high-resolution timer wakeups while the engine still has >= 80% of
 * a packet to write the next one. The frame clock is exact for any period. */
static inline ULONG PnWaveTickPeriodHns(ULONG NotifyIntervalHns)
{
    return (NotifyIntervalHns >= 50000) ? 20000 : 10000;
}

/* After this many consecutive unwritten packets the engine is not following the
 * SetWritePacket protocol this stream expects (or has stopped feeding it): stop
 * substituting silence rather than mute the endpoint. The check keeps watching and re-arms
 * after PN_WAVE_STARVE_REARM consecutive packets written on time, so one long engine stall
 * does not turn it off for the rest of the run (finding #21). */
#define PN_WAVE_STARVE_GIVE_UP      8
#define PN_WAVE_STARVE_REARM        50

typedef struct _PN_WAVE_STARVATION {
    ULONGLONG DecidedPacket;    /* linear packet index + 1 of the last decision, 0 = none */
    ULONG     Starved;          /* packets replaced by silence */
    ULONG     Streak;           /* consecutive starved packets */
    ULONG     OnTime;           /* consecutive packets written on time since giving up */
    BOOLEAN   Silent;
    BOOLEAN   Disabled;         /* gave up until the engine writes on time again */
} PN_WAVE_STARVATION;

/* Event-driven packet mode: the engine reports each written packet through SetWritePacket
 * and must have written packet N before playback reaches it. A packet that is not written
 * when playback enters it holds the previous buffer cycle; it is played as silence for its
 * whole duration (a late write of the following packet does not revive it) and counted
 * once - also the packet that makes the check give up. Packet numbers are 32-bit and wrap.
 * Call it for every packet, also after giving up. */
static inline BOOLEAN PnWavePacketSilent(PN_WAVE_STARVATION *S, ULONGLONG Packet, BOOLEAN WritePacketUsed, ULONG LastWrittenPacket)
{
    if (S->DecidedPacket == Packet + 1) return S->Silent;
    BOOLEAN late = WritePacketUsed && (LONG)((ULONG)Packet - LastWrittenPacket) > 0;
    S->DecidedPacket = Packet + 1;
    if (S->Disabled) {
        S->Silent = FALSE;
        S->OnTime = late ? 0 : S->OnTime + 1;
        if (S->OnTime >= PN_WAVE_STARVE_REARM) { S->Disabled = FALSE; S->Streak = 0; S->OnTime = 0; }
        return FALSE;
    }
    S->Silent = late;
    if (late) {
        S->Starved++;
        if (++S->Streak >= PN_WAVE_STARVE_GIVE_UP) { S->Disabled = TRUE; S->OnTime = 0; }
    } else {
        S->Streak = 0;
    }
    return late;
}

/* A late render tick (a delayed timer DPC) can cross two packet boundaries before the engine
 * is told about the first: the second packet was never requested, yet it was judged starved
 * and silenced (finding #20). Instead playback stops at the start of a packet the engine has
 * not been notified to write (Packet >= NotifiedPacket + 2), the engine is notified, and the
 * frames due are owed to the next ticks; the hold ends once the packet is written, after
 * PN_WAVE_LATE_HOLD_HNS, or when more than one packet is owed (then the packet starves as
 * before). The pump ring absorbs the short delay. */
#define PN_WAVE_LATE_HOLD_HNS       80000ull   /* 8 ms after the notification */

typedef struct _PN_WAVE_LATE_HOLD {
    ULONGLONG Packet;      /* linear packet index + 1 held at, 0 = none */
    ULONGLONG SinceHns;
} PN_WAVE_LATE_HOLD;

/* At the first frame of Packet: TRUE = do not play it yet. */
static inline BOOLEAN PnWaveLateHold(PN_WAVE_LATE_HOLD *H, ULONGLONG Packet, ULONGLONG NotifiedPacket, ULONG LastWrittenPacket,
                                     ULONGLONG NowHns, ULONGLONG OwedBytes, ULONG MaxOwedBytes)
{
    BOOLEAN written = (LONG)((ULONG)Packet - LastWrittenPacket) <= 0;
    if (H->Packet == Packet + 1) {
        if (written || NowHns - H->SinceHns >= PN_WAVE_LATE_HOLD_HNS || OwedBytes > MaxOwedBytes) { H->Packet = 0; return FALSE; }
        return TRUE;
    }
    if (written || Packet < NotifiedPacket + 2 || OwedBytes > MaxOwedBytes) return FALSE;
    H->Packet = Packet + 1;
    H->SinceHns = NowHns;
    return TRUE;
}

/* Playback start (findings #23/#24): the pump starts only after the AVDTP START round trip
 * (plus a sniff exit), 130-450 ms on the Sony. Rendering meanwhile advanced the WaveRT clock
 * and its audio was flushed at the start: the onset of every sound after idle was lost. The
 * clock holds instead while a start is pending, for at most PN_WAVE_START_HOLD_MS, so the
 * audio is delayed like a device's start-up latency, not lost; the WaveRT position, the
 * presentation clock and A/V sync stay consistent. HoldUntilHns 0 = no start pending. */
#define PN_WAVE_START_HOLD_MS       500u

static inline BOOLEAN PnWaveStartHold(LONGLONG NowHns, LONGLONG HoldUntilHns, BOOLEAN PumpStreaming)
{
    return !PumpStreaming && HoldUntilHns != 0 && NowHns < HoldUntilHns;
}

/* Edges of substituted silence (finding #4): a starved packet starts with a short faded
 * mirror of the audio before it and the audio after it fades in, so the gap has no hard
 * steps. Frames of each fade: 2 ms, within one packet and the stream's scratch buffer. */
#define PN_WAVE_FADE_US             2000u

static inline ULONG PnWaveFadeFrames(ULONG Rate, ULONG PacketFrames, ULONG ScratchFrames)
{
    ULONG f = (ULONG)((ULONGLONG)Rate * PN_WAVE_FADE_US / 1000000u);
    if (PacketFrames && f > PacketFrames) f = PacketFrames;
    if (f > ScratchFrames) f = ScratchFrames;
    return f;
}

typedef struct _PN_WAVE_PRESENTATION {
    ULONGLONG LastFrames;         /* last reported position, monotonic within a run */
    ULONGLONG LastQpc;
    ULONGLONG SmoothedLatencyNs;  /* slew-limited latency subtracted from the position */
    BOOLEAN   Valid;              /* FALSE at the start of a run */
    BOOLEAN   Snap;               /* a new pipeline epoch: take its latency at once */
    ULONG     Epoch;
} PN_WAVE_PRESENTATION;

/* The pipeline behind the latency was restarted (the pump started or stopped a stream:
 * the ring was flushed). The latency of the old pipeline says nothing about the new one,
 * so the next position takes the new latency at once instead of slewing toward it at
 * 1 %/s, which carried a full-ring 600 ms into playback for a minute (review #7). The
 * position still never moves backwards. */
static inline VOID PnWavePresentationSetEpoch(PN_WAVE_PRESENTATION *S, ULONG Epoch)
{
    if (S->Valid && S->Epoch != Epoch) S->Snap = TRUE;
    S->Epoch = Epoch;
}

/* Presentation position in frames at NowQpc (the QPC reported with it).
 * TickFrames/TickQpc: frames consumed by the render clock at its last tick. While running
 * the position is interpolated from that tick to NowQpc at the nominal rate (at most
 * 20 ms ahead), then the smoothed transport latency is subtracted. The latency follows
 * LatencyUs at no more than PN_WAVE_LATENCY_SLEW_PPM, so per-write steps of the estimate
 * and delay reports are applied gradually; the result never moves backwards. */
static inline ULONGLONG PnWavePresentationFrames(PN_WAVE_PRESENTATION *S, ULONGLONG TickFrames, ULONGLONG TickQpc, BOOLEAN Running,
                                                 ULONGLONG NowQpc, ULONGLONG QpcFrequency, ULONG Rate, ULONG LatencyUs)
{
    ULONGLONG interp = TickFrames;
    if (Running && QpcFrequency && NowQpc > TickQpc) {
        ULONGLONG d = NowQpc - TickQpc;
        if (d > QpcFrequency / 50) d = QpcFrequency / 50;
        interp += d * Rate / QpcFrequency;
    }
    if (LatencyUs > PN_WAVE_MAX_LATENCY_US) LatencyUs = PN_WAVE_MAX_LATENCY_US;
    ULONGLONG target = (ULONGLONG)LatencyUs * 1000ull;
    if (!S->Valid || S->Snap) {
        S->SmoothedLatencyNs = target;
        S->Snap = FALSE;
    } else if (QpcFrequency && NowQpc > S->LastQpc) {
        ULONGLONG d = NowQpc - S->LastQpc;
        if (d > QpcFrequency) d = QpcFrequency;
        ULONGLONG maxStep = (d * 1000000ull / QpcFrequency) * PN_WAVE_LATENCY_SLEW_PPM / 1000ull;   /* us * ppm -> ns */
        if (target > S->SmoothedLatencyNs) S->SmoothedLatencyNs += PN_MIN(target - S->SmoothedLatencyNs, maxStep);
        else S->SmoothedLatencyNs -= PN_MIN(S->SmoothedLatencyNs - target, maxStep);
    }
    ULONGLONG lat = (S->SmoothedLatencyNs / 1000ull) * Rate / 1000000ull;
    ULONGLONG pos = interp > lat ? interp - lat : 0;
    if (S->Valid && pos < S->LastFrames) pos = S->LastFrames;
    S->LastFrames = pos;
    if (NowQpc > S->LastQpc || !S->Valid) S->LastQpc = NowQpc;
    S->Valid = TRUE;
    return pos;
}

/* PCM that holds a signal: any non-zero byte (integer and float silence are all zero).
 * Bounded by Length (one tick's segment). Review round 4 #7. */
static inline BOOLEAN PnPcmHasSignal(const UCHAR *Data, ULONG Length)
{
    for (ULONG i = 0; i < Length; i++) if (Data[i]) return TRUE;
    return FALSE;
}

/* The volume and mute nodes advertise one (master) channel. */
static inline BOOLEAN PnKsChannelIsMaster(ULONG Channel)
{
    return Channel == 0 || Channel == 0xFFFFFFFFu;
}

/* Clamp to [MinDb16, MaxDb16] and snap to the advertised step (nearest, ties up),
 * counting steps from MinDb16 as KSPROPERTY_STEPPING_LONG does. */
static inline LONG PnVolumeSnapDb16(LONG Db16, LONG MinDb16, LONG MaxDb16, LONG StepDb16)
{
    if (Db16 > MaxDb16) Db16 = MaxDb16;
    if (Db16 < MinDb16) Db16 = MinDb16;
    if (StepDb16 <= 0) return Db16;
    ULONG offset = (ULONG)Db16 - (ULONG)MinDb16;
    ULONG steps = (offset + (ULONG)StepDb16 / 2) / (ULONG)StepDb16;
    LONGLONG v = (LONGLONG)MinDb16 + (LONGLONG)steps * StepDb16;
    if (v > MaxDb16) v -= StepDb16;
    return (LONG)v;
}
