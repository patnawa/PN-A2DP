/* Bounded PCM conversion shared by the pump and user-mode regression tests.
 * SPDX-License-Identifier: MIT */
#pragma once
ULONG PnVolumeGainQ15(LONG Db16);
ULONG PnConvertPcm(const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                   ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                   ULONG Target, ULONG TargetChannels, ULONG GainQ15);

/* 2:1 half-band decimator (engine at twice the codec rate): 103-tap FIR, Q28, every
 * second coefficient zero except the centre (0.5). Passband ripple < 0.001 dB up to
 * 0.45 fs_out, attenuation >= 80 dB from 0.55 fs_out, DC gain exactly 1. */
#define PN_HB_TAPS    103
#define PN_HB_CENTER  51
#define PN_HB_HALF    26        /* non-zero coefficients on each side of the centre */
extern const LONG g_PnHalfbandQ28[PN_HB_HALF];   /* taps at CENTER +- (2j + 1) */

/* Conversion state carried from block to block by one stream (fixed point only).
 * Gain changes ramp linearly across one block instead of stepping. A latency trim
 * leaves FadeFrames of the audio that would have followed the cut in FadeOut (ring
 * format); the next frames converted are crossfaded from it. Zero it at stream start,
 * then set Dither / Decimate for the stream. */
typedef struct _PN_PCM_STATE {
    BOOLEAN      GainValid;     /* FALSE: start at the requested gain without a ramp */
    ULONG        GainQ15;       /* gain reached at the end of the previous block */
    const UCHAR *FadeOut;
    ULONG        FadeFrames;    /* 0 = no crossfade pending */
    ULONG        FadePos;       /* frames already crossfaded (input frames) */
    /* TPDF dither for S16 output whenever the value is not already a 16-bit sample
     * (gain below unity, a wider source, decimation or a crossfade); never when muted
     * and never in a block of digital silence (all-zero input) */
    BOOLEAN      Dither;
    ULONG        DitherSeed;    /* xorshift32 state; 0 starts from a fixed seed */
    /* 2 = decimate 2:1 (FadeOut and the input stay at the engine rate), else off */
    ULONG        Decimate;
    ULONG        HbPhase;       /* input frames of the current output pair already filtered (0/1) */
    ULONG        HbPos;         /* oldest sample of the window */
    LONGLONG     HbHistory[2][2 * PN_HB_TAPS];   /* per channel, written twice for a linear window; S32 units, unsaturated */
    /* float input: a NaN or +-Inf sample repeats the channel's last finite sample, fading to
     * silence over PN_PCM_NONFINITE_HOLD samples of a run (finding #6); per input channel */
    LONGLONG     LastFinite[2];
    ULONG        NonFiniteRun[2];
} PN_PCM_STATE;

#define PN_PCM_NONFINITE_HOLD 32u

/* PnConvertPcm with State (may be NULL: then identical to PnConvertPcm). With Decimate
 * set, InBytes holds engine-rate frames and the result is codec-rate frames. */
ULONG PnConvertPcmEx(PN_PCM_STATE *State, const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                     ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                     ULONG Target, ULONG TargetChannels, ULONG GainQ15);

/* A stream's conversion state at its start. FadeIn: the first PCM is mid-programme (audio
 * before it was discarded), so the first block ramps in from silence over its length instead
 * of stepping from the receiver's silence to it (findings #5, #25). Set Dither afterwards. */
VOID PnPcmStateReset(PN_PCM_STATE *State, ULONG Decimate, BOOLEAN FadeIn);

/* Raised-cosine fade of interleaved PCM in its own format, in place (integer only; any IRQL,
 * the buffer must be resident). Frame i is frame k = RampPos + i of a RampLen-frame ramp, gain
 * sin^2(pi/2 * x): up x = (k + 1) / (RampLen + 1), frames past the ramp left alone; down
 * x = (RampLen - k) / (RampLen + 1), frames past the ramp zeroed. Used where audio is cut
 * (starved packets, the end of a stream): a step becomes a short fade. */
VOID PnPcmRamp(PUCHAR Pcm, ULONG Frames, ULONG Channels, ULONG ContainerBits, ULONG ValidBits, BOOLEAN IsFloat,
               ULONG RampPos, ULONG RampLen, BOOLEAN Up);
