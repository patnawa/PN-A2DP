/* Bounded PCM conversion shared by the pump and user-mode regression tests.
 * SPDX-License-Identifier: MIT */
#pragma once
ULONG PnVolumeGainQ15(LONG Db16);
ULONG PnConvertPcm(const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                   ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                   ULONG Target, ULONG TargetChannels, ULONG GainQ15);

/* ---- Sample-rate conversion (0.1.0.22 #1) ---------------------------------------------------
 * Fixed point only (kernel): Q28 coefficients, 64-bit accumulators, linear phase. Engine (WaveRT)
 * rates 44.1, 48, 88.2 and 96 kHz feed codec rates 44.1 and 48 kHz (PnPcmRatePlan):
 *   - equal rates: the native path, untouched (bit-exact with 0.1.0.21);
 *   - exactly 2:1 (96 -> 48, 88.2 -> 44.1 kHz): a 2:1 decimation FIR;
 *   - 44.1 <-> 48 kHz: one polyphase stage (L/M = 160/147 or 147/160);
 *   - 88.2 -> 48 and 96 -> 44.1 kHz: a short polyphase stage into the other family's double rate
 *     (88.2 -> 96, 96 -> 88.2 kHz), then that rate's 2:1 FIR (review of 0.1.0.22: a third of the
 *     multiplies of the first design's 2:1-first order, the kernel's CPU budget).
 * Alias-free band plan (owner decision, 14 October 2026): passband 0..20 kHz flat within
 * 0.000015 dB per stage (0.000021 dB through a cascade); stopband from the lower of the input and
 * output Nyquist frequencies, 22.05 kHz whenever 44.1 kHz is involved and 24 kHz for 96 -> 48 and
 * 88.2 -> 48 kHz, at least 122.0 dB down (160/147 and 147/160 122.9 dB, the 88.2 kHz 2:1 FIR
 * 122.7 dB, the 96 kHz one 122.0 dB). In a cascade the 2:1 stage protects the final band on its
 * own and the polyphase stage stops, at least 121.7 dB down, everything that would fold onto it at
 * its output rate (from 0.75 of that rate: 72 / 66.15 kHz). No alias or image of any input
 * frequency comes out above -121.7 dB (generator: -122.0 dB worst over every conversion). The
 * tables are generated (resample_coeffs.h, build/gen-resampler-coeffs.py, which measures the
 * integers and checks every conversion's aliases; pcm.cpp includes them once) and non-paged;
 * core_test measures exactly these integers and runs the converter. */

/* A 2:1 decimation FIR: Taps (odd) coefficients symmetric about Center = (Taps - 1) / 2,
 * evaluated once per input pair over the window ending with the pair's second frame, so output m
 * stands for input frame 2m + 1 - Center (group delay Center - 1 input frames; its input is the
 * engine rate, or the polyphase stage's output in a cascade). Coef[0] is the
 * centre tap, Coef[k] the pair at Center +- k (Q28); Coef[0] + 2 * sum(Coef[1..]) is exactly 2^28
 * (DC gain exactly 1) and the sum of |taps| stays below 3.5 * 2^28 (the overflow bound). */
typedef struct _PN_DEC2_TABLE {
    ULONG        Taps;
    ULONG        Center;
    const LONG  *Coef;          /* [Center + 1], Q28 */
} PN_DEC2_TABLE;
#define PN_DEC2_96_TAPS    195      /* passband 20 kHz, stopband from 24 kHz at 96 kHz */
#define PN_DEC2_88_TAPS    347      /* passband 20 kHz, stopband from 22.05 kHz at 88.2 kHz */
#define PN_DEC2_MAX_TAPS   347
extern const PN_DEC2_TABLE g_PnDec96;            /* 96 -> 48 kHz, and stage 2 of 88.2 -> 48 kHz */
extern const PN_DEC2_TABLE g_PnDec88;            /* 88.2 -> 44.1 kHz, and stage 2 of 96 -> 44.1 kHz */
extern const LONG g_PnDec96Q28[(PN_DEC2_96_TAPS + 1) / 2];
extern const LONG g_PnDec88Q28[(PN_DEC2_88_TAPS + 1) / 2];

/* A polyphase stage: L output frames for every M input frames. Output n uses phase
 * p = (n * M) mod L, Taps inputs ending with input floor(n * M / L), oldest first; every phase
 * sums to exactly 2^28 and its sum of |taps| stays below 3.5 * 2^28 (the overflow bound). */
typedef struct _PN_RS_TABLE {
    ULONG        L, M;
    ULONG        Taps;          /* per phase */
    ULONG        CenterTwice;   /* N - 1 of the prototype at F = Fin * L: group delay CenterTwice / (2 F) */
    const LONG  *Coef;          /* [L][Taps], Q28 */
} PN_RS_TABLE;
#define PN_RS_MAX_TAPS     188
#define PN_RS_44TO48_TAPS  173      /* stopband from 22.05 kHz */
#define PN_RS_48TO44_TAPS  188      /* stopband from 22.05 kHz */
#define PN_RS_88TO96_TAPS  17       /* stopband from 72 kHz (what folds onto 0..24 kHz at 96 kHz) */
#define PN_RS_96TO88_TAPS  21       /* stopband from 66.15 kHz (what folds onto 0..22.05 kHz at 88.2 kHz) */
extern const PN_RS_TABLE g_PnRs44to48;           /* L 160, M 147 */
extern const PN_RS_TABLE g_PnRs48to44;           /* L 147, M 160 */
extern const PN_RS_TABLE g_PnRs88to96;           /* L 160, M 147: stage 1 of 88.2 -> 48 kHz */
extern const PN_RS_TABLE g_PnRs96to88;           /* L 147, M 160: stage 1 of 96 -> 44.1 kHz */
extern const LONG g_PnRs160x147Q28[160][PN_RS_44TO48_TAPS];
extern const LONG g_PnRs147x160Q28[147][PN_RS_48TO44_TAPS];
extern const LONG g_PnRs88to96Q28[160][PN_RS_88TO96_TAPS];
extern const LONG g_PnRs96to88Q28[147][PN_RS_96TO88_TAPS];

/* The conversion from one engine rate to one codec rate. */
typedef struct _PN_PCM_RATE_PLAN {
    ULONG                EngineRate, CodecRate;
    ULONG                Decimate;          /* 2: a 2:1 stage (Dec), else 1 */
    const PN_DEC2_TABLE *Dec;               /* the 2:1 stage's filter when Decimate is 2, else NULL */
    const PN_RS_TABLE   *Rs;                /* the polyphase stage, NULL = none; first in a cascade */
    ULONG                DelayEngineFrames; /* latency the stages add, engine frames (rounded) */
} PN_PCM_RATE_PLAN;

/* Conversion state carried from block to block by one stream (fixed point only).
 * Gain changes ramp linearly across one block instead of stepping. A latency trim
 * leaves FadeFrames of the audio that would have followed the cut in FadeOut (ring
 * format); the next frames converted are crossfaded from it. Reset it at stream start
 * (PnPcmStateResetRate), then set Dither for the stream. */
typedef struct _PN_PCM_STATE {
    BOOLEAN      GainValid;     /* FALSE: start at the requested gain without a ramp */
    ULONG        GainQ15;       /* gain reached at the end of the previous block */
    const UCHAR *FadeOut;
    ULONG        FadeFrames;    /* 0 = no crossfade pending */
    ULONG        FadePos;       /* frames already crossfaded (input frames) */
    /* TPDF dither for S16 output whenever the value is not already a 16-bit sample
     * (gain below unity, a wider source, rate conversion or a crossfade); never when muted
     * and never in a block of digital silence (all-zero input and an empty filter history) */
    BOOLEAN      Dither;
    ULONG        DitherSeed;    /* xorshift32 state; 0 starts from a fixed seed */
    /* 2 = decimate 2:1 (FadeOut and the input stay at the engine rate), else off */
    ULONG        Decimate;
    /* the 2:1 stage's filter (Decimate 2); NULL takes g_PnDec88, whose band edges in units of its
     * input rate (passband 0.2268, stopband from 0.25) hold at 96 kHz as well. In a cascade its
     * input is the polyphase stage's output. */
    const PN_DEC2_TABLE *Dec;
    ULONG        DecPhase;      /* inputs of the current output pair already filtered (0/1) */
    ULONG        DecPos;        /* oldest sample of the window */
    /* per channel, written twice for a linear window; S32 units, not clamped to S32 (|v| <= 2^33:
     * the input decode's limit, the crossfade and the ramp-in mixing bounded values; in a cascade
     * the polyphase stage's output, clamped to +-2^33 before it) */
    LONGLONG     DecHistory[2][2 * PN_DEC2_MAX_TAPS];
    /* the polyphase stage (NULL = none); it runs at the engine rate, and with Decimate 2 its output
     * feeds the 2:1 stage */
    const PN_RS_TABLE *Rs;
    ULONG        RsPhase;       /* (n * M) mod L of the next output n */
    ULONG        RsPending;     /* inputs to push before the next output (1 after a reset; 0: due) */
    ULONG        RsPos;         /* oldest sample of the window */
    /* per channel, written twice for a linear window, as DecHistory; S32 units, |v| <= 2^33 (the
     * input decode's limit) */
    LONGLONG     RsHistory[2][2 * PN_RS_MAX_TAPS];
    BOOLEAN      Round24;       /* S24 target: round to 24 bits instead of truncating (converted streams) */
    /* a converted stream starting mid-programme: the first call's input ramps in from silence
     * before the filters (an output ramp would start a filter delay before the onset leaves the
     * filters, which then steps out of an empty history at the ramp's level) */
    BOOLEAN      RampIn;
    /* float input: a NaN or +-Inf sample repeats the channel's last finite sample, fading to
     * silence over PN_PCM_NONFINITE_HOLD samples of a run (finding #6); per input channel */
    LONGLONG     LastFinite[2];
    ULONG        NonFiniteRun[2];
} PN_PCM_STATE;

#define PN_PCM_NONFINITE_HOLD 32u

/* A latency trim whose crossfade has not started (nothing converted since the previous trim:
 * a stall that keeps the TX queue full trims again every ~(max - target)) keeps that crossfade
 * and its source. The pending FadeOut continues the audio played last; the head of the newly
 * dropped audio does not (it follows audio that was itself dropped): taking it would step at
 * the splice (-10..-15 dB re tone, found with the Low latency profile in the quality harness). */
static inline BOOLEAN PnPcmFadePending(const PN_PCM_STATE *S)
{
    return S->FadeOut && S->FadeFrames && S->FadePos == 0;
}

/* PnConvertPcm with State (may be NULL: then identical to PnConvertPcm). With a rate plan
 * (PnPcmStateResetRate), InBytes holds engine-rate frames and the result is codec-rate frames:
 * at most OutCap's worth. When every output the input completes fits, all of the input is
 * consumed (frames it does not complete wait in the filter history); otherwise exactly the
 * input of the frames written. The pump hands over PnPcmInputFramesFor(State, n) frames with
 * room for exactly n. */
ULONG PnConvertPcmEx(PN_PCM_STATE *State, const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                     ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                     ULONG Target, ULONG TargetChannels, ULONG GainQ15);

/* The conversion from EngineRate to CodecRate; FALSE when none bridges the pair (Plan zeroed). */
BOOLEAN PnPcmRatePlan(ULONG EngineRate, ULONG CodecRate, PN_PCM_RATE_PLAN *Plan);

/* A stream's conversion state at its start, for Plan (NULL: native). FadeIn: the first PCM is
 * mid-programme (audio before it was discarded), so the first block ramps in from silence over
 * its length instead of stepping from the receiver's silence to it (findings #5, #25). Set
 * Dither afterwards. Converted streams round S24 output (Round24). */
VOID PnPcmStateResetRate(PN_PCM_STATE *State, const PN_PCM_RATE_PLAN *Plan, BOOLEAN FadeIn);
/* The same with only a decimation factor (2: the 2:1 stage with g_PnDec88, else native); kept for the
 * tests. */
VOID PnPcmStateReset(PN_PCM_STATE *State, ULONG Decimate, BOOLEAN FadeIn);

/* The engine frames that complete exactly OutFrames more output frames from State (exact:
 * the converter then consumes all of them and leaves the 2:1 stage between pairs). */
ULONG PnPcmInputFramesFor(const PN_PCM_STATE *State, ULONG OutFrames);
/* An upper bound of PnPcmInputFramesFor over every state of Plan (buffer sizing, wake level). */
ULONG PnPcmMaxInputFrames(const PN_PCM_RATE_PLAN *Plan, ULONG OutFrames);

/* Raised-cosine fade of interleaved PCM in its own format, in place (integer only; any IRQL,
 * the buffer must be resident). Frame i is frame k = RampPos + i of a RampLen-frame ramp, gain
 * sin^2(pi/2 * x): up x = (k + 1) / (RampLen + 1), frames past the ramp left alone; down
 * x = (RampLen - k) / (RampLen + 1), frames past the ramp zeroed. Used where audio is cut
 * (starved packets, the end of a stream): a step becomes a short fade. */
VOID PnPcmRamp(PUCHAR Pcm, ULONG Frames, ULONG Channels, ULONG ContainerBits, ULONG ValidBits, BOOLEAN IsFloat,
               ULONG RampPos, ULONG RampLen, BOOLEAN Up);
