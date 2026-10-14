/* SPDX-License-Identifier: MIT */
#include "codec_core.h"
#include "pcm.h"
#include "resample_coeffs.h"   /* the coefficient tables (generated; included here only) */

static_assert(PN_RS_160X147_TAPS == PN_RS_44TO48_TAPS && PN_RS_147X160_TAPS == PN_RS_48TO44_TAPS, "pcm.h matches the generated tables");
static_assert(PN_RS_88X96_TAPS == PN_RS_88TO96_TAPS && PN_RS_96X88_TAPS == PN_RS_96TO88_TAPS, "pcm.h matches the generated cascade stages");
static_assert(PN_RS_44TO48_TAPS <= PN_RS_MAX_TAPS && PN_RS_48TO44_TAPS <= PN_RS_MAX_TAPS && PN_RS_88TO96_TAPS <= PN_RS_MAX_TAPS &&
              PN_RS_96TO88_TAPS <= PN_RS_MAX_TAPS, "the history holds every window");
static_assert(PN_RS_DEC96_TAPS == PN_DEC2_96_TAPS && PN_RS_DEC88_TAPS == PN_DEC2_88_TAPS, "pcm.h matches the generated 2:1 FIRs");
static_assert(PN_DEC2_96_TAPS % 2 == 1 && PN_DEC2_88_TAPS % 2 == 1 && PN_DEC2_96_TAPS <= PN_DEC2_MAX_TAPS && PN_DEC2_88_TAPS <= PN_DEC2_MAX_TAPS &&
              sizeof(g_PnDec96Q28) / sizeof(g_PnDec96Q28[0]) == (PN_DEC2_96_TAPS + 1) / 2 &&
              sizeof(g_PnDec88Q28) / sizeof(g_PnDec88Q28[0]) == (PN_DEC2_88_TAPS + 1) / 2, "2:1 FIR layout: odd, stored from the centre");

const PN_DEC2_TABLE g_PnDec96 = { PN_DEC2_96_TAPS, PN_DEC2_96_TAPS / 2, g_PnDec96Q28 };
const PN_DEC2_TABLE g_PnDec88 = { PN_DEC2_88_TAPS, PN_DEC2_88_TAPS / 2, g_PnDec88Q28 };
const PN_RS_TABLE g_PnRs44to48 = { 160, 147, PN_RS_160X147_TAPS, PN_RS_160X147_CENTER_TWICE, &g_PnRs160x147Q28[0][0] };
const PN_RS_TABLE g_PnRs48to44 = { 147, 160, PN_RS_147X160_TAPS, PN_RS_147X160_CENTER_TWICE, &g_PnRs147x160Q28[0][0] };
const PN_RS_TABLE g_PnRs88to96 = { 160, 147, PN_RS_88X96_TAPS, PN_RS_88X96_CENTER_TWICE, &g_PnRs88to96Q28[0][0] };
const PN_RS_TABLE g_PnRs96to88 = { 147, 160, PN_RS_96X88_TAPS, PN_RS_96X88_CENTER_TWICE, &g_PnRs96to88Q28[0][0] };

ULONG PnVolumeGainQ15(LONG Db16)
{
    if (Db16 >= 0) return 32768;
    if (Db16 <= -96 * 0x10000) return 0;
    /* 0.5 dB steps, matching the topology node. No floating point in the kernel.
     * Entries are round(32768 * 10^(-index/40)). */
    static const USHORT gain[192] = {
        32768, 30935, 29205, 27571, 26029, 24573, 23198, 21900, 20675, 19519, 18427, 17396, 16423, 15504, 14637, 13818,
        13045, 12315, 11627, 10976, 10362, 9783, 9235, 8719, 8231, 7771, 7336, 6925, 6538, 6172, 5827, 5501,
        5193, 4903, 4629, 4370, 4125, 3894, 3677, 3471, 3277, 3093, 2920, 2757, 2603, 2457, 2320, 2190,
        2068, 1952, 1843, 1740, 1642, 1550, 1464, 1382, 1305, 1232, 1163, 1098, 1036, 978, 924, 872,
        823, 777, 734, 693, 654, 617, 583, 550, 519, 490, 463, 437, 413, 389, 368, 347,
        328, 309, 292, 276, 260, 246, 232, 219, 207, 195, 184, 174, 164, 155, 146, 138,
        130, 123, 116, 110, 104, 98, 92, 87, 82, 78, 73, 69, 65, 62, 58, 55,
        52, 49, 46, 44, 41, 39, 37, 35, 33, 31, 29, 28, 26, 25, 23, 22,
        21, 20, 18, 17, 16, 16, 15, 14, 13, 12, 12, 11, 10, 10, 9, 9,
        8, 8, 7, 7, 7, 6, 6, 6, 5, 5, 5, 4, 4, 4, 4, 3,
        3, 3, 3, 3, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1
    };
    return gain[(ULONG)(-Db16) / 0x8000];
}

/* S32 units, carried 64-bit from the input to the output (finding #3): a float over full scale,
 * a crossfade and the rate converter's overshoot keep their value until the gain has been
 * applied, and the result saturates once when it is stored. Float input is held to +-4.0
 * (+12 dBFS), and so is a cascade's polyphase output that feeds the 2:1 stage (its overshoot is
 * up to 1.69x: unclamped, 1.69 * 2^33 * 2.45 * 2^28 would come close to 2^63, and any table's
 * sum within the generator's bound could pass it). With every 2:1 FIR's and every phase's sum of
 * |taps| at most 2.75 * 2^28 (the generator asserts 3.5 * 2^28; any sum below 4 * 2^28 would do),
 * no 64-bit accumulator can overflow (2^33 * 3.5 * 2^28 < 2^63). */
#define PN_PCM_FLOAT_LIMIT   (1ll << 33)

static LONGLONG ClampFloatLimit(LONGLONG v)
{
    return v > PN_PCM_FLOAT_LIMIT ? PN_PCM_FLOAT_LIMIT : v < -PN_PCM_FLOAT_LIMIT ? -PN_PCM_FLOAT_LIMIT : v;
}

static LONGLONG ReadSample(const UCHAR *s, ULONG bytes, ULONG valid, BOOLEAN floating)
{
    ULONG bits = 0;
    RtlCopyMemory(&bits, s, bytes);
    if (floating) {
        // Decode IEEE binary32 directly: +1.0f cannot be converted through float * INT_MAX
        // because INT_MAX rounds to 2^31 in binary32. NaN and +-Inf become silence: a
        // misbehaving source must not become a full-scale pop.
        ULONG magnitude = bits & 0x7fffffff;
        if (magnitude >= 0x7f800000) return 0;
        ULONG exponent = magnitude >> 23;
        if (exponent < 96) return 0; // magnitude below one S32 unit
        LONGLONG value;
        if (exponent >= 129) value = PN_PCM_FLOAT_LIMIT;   /* 4.0 and above */
        else {
            ULONGLONG mantissa = (magnitude & 0x7fffff) | 0x800000;
            value = (LONGLONG)(exponent >= 119 ? (mantissa << (exponent - 119)) : (mantissa >> (119 - exponent)));
        }
        return (bits >> 31) ? -value : value;
    }
    bits <<= 32 - bytes * 8;
    if (valid < bytes * 8) bits &= ~((1u << (32 - valid)) - 1);
    return (LONG)bits;
}

static LONG SaturateS32(LONGLONG v)
{
    return v > 2147483647ll ? 2147483647L : v < -2147483647ll - 1 ? (-2147483647L - 1) : (LONG)v;
}

/* NaN or +-Inf (IEEE binary32 bits). */
static BOOLEAN F32NonFinite(const UCHAR *s)
{
    ULONG bits;
    RtlCopyMemory(&bits, s, 4);
    return (bits & 0x7fffffff) >= 0x7f800000;
}

/* Finding #6: a non-finite float sample (ReadSample made it 0) repeats channel Channel's last
 * finite sample instead of dropping to zero for one sample, a broadband click in a loud
 * programme; a run of them fades linearly to silence over PN_PCM_NONFINITE_HOLD samples, so a
 * source stuck at NaN goes quiet rather than holding a DC level. */
static LONGLONG ConcealNonFinite(_Inout_ PN_PCM_STATE *S, ULONG Channel, const UCHAR *s, LONGLONG Value)
{
    if (!F32NonFinite(s)) {
        S->LastFinite[Channel] = Value;
        S->NonFiniteRun[Channel] = 0;
        return Value;
    }
    if (S->NonFiniteRun[Channel] <= PN_PCM_NONFINITE_HOLD) S->NonFiniteRun[Channel]++;
    ULONG run = S->NonFiniteRun[Channel];   /* 1 = the first: held at full level */
    if (run > PN_PCM_NONFINITE_HOLD) return 0;
    return S->LastFinite[Channel] * (LONGLONG)(PN_PCM_NONFINITE_HOLD + 1 - run) / (LONGLONG)PN_PCM_NONFINITE_HOLD;
}

/* One output sample: channel c of frame f, mixed down when stereo becomes mono. */
static LONGLONG ReadFrameSample(const UCHAR *Base, ULONG Frame, ULONG Channel, ULONG Channels, ULONG TargetChannels,
                                ULONG InBps, ULONG ValidBits, BOOLEAN IsFloat)
{
    const UCHAR *s = Base + (Frame * Channels + PN_MIN(Channel, Channels - 1)) * InBps;
    LONGLONG v = ReadSample(s, InBps, ValidBits, IsFloat);
    if (Channels == 2 && TargetChannels == 1)
        v = (v + ReadSample(s + InBps, InBps, ValidBits, IsFloat)) / 2;
    return v;
}

/* The 2:1 stage's filter: the plan's, or g_PnDec88 for a state set up without one (pcm.h). */
static const PN_DEC2_TABLE *DecTable(_In_ const PN_PCM_STATE *S)
{
    return S->Dec ? S->Dec : &g_PnDec88;
}

/* The two MAC loops below are the converter's whole cost. They are scalar by construction
 * (#pragma loop(no_vector)): 64-bit products have no SSE2 form, and the auto-vectorizer would add
 * an AVX-512 path dispatched on __isa_available, which the kernel never sets (dead code in the
 * driver image, and EVEX code that would need KeSaveExtendedProcessorState if anything ever set
 * it). So user mode (core_test, the quality harness) runs and times exactly the code the driver
 * executes. Both channels share each coefficient load (channel 1's window lies at a fixed
 * distance from channel 0's in the state's [2][...] array); the 2:1 loop takes two taps per
 * iteration into two partial sums of disjoint terms, each bounded as the whole sum is, so no
 * partial sum overflows and the exact integer result does not depend on the order. */

/* One 2:1 output sample per channel from the window ending with the pair's second frame. Not
 * saturated: the overshoot of a full-scale input survives a gain below unity (finding #3).
 * Kaiser-windowed symmetric FIR (resample_coeffs.h), DC sum exact: Coef[0] + 2 * sum = 2^28; the
 * pair at Center +- k shares its tap. |w| <= 2^33 (a pair's sum 2^34) and the sum of |taps| is at
 * most 2.45 * 2^28 (generator: below 3.5 * 2^28): |acc| < 2^33 * 3.5 * 2^28 < 2^63, no overflow. */
static VOID DecOutputs(_In_ const PN_PCM_STATE *S, _In_ const PN_DEC2_TABLE *D, ULONG Channels, _Out_writes_(Channels) LONGLONG *Out)
{
    const SIZE_T other = 2 * PN_DEC2_MAX_TAPS;   /* channel 1's window, at a fixed distance from channel 0's */
    const ULONG n = D->Center;
    const LONG *h = D->Coef + n;                   /* walks down: Coef[Center - j] pairs lo[j] and hi[-j] */
    const LONGLONG *lo = &S->DecHistory[0][S->DecPos], *hi = lo + 2 * n;   /* oldest, newest */
    LONGLONG a = (LONGLONG)D->Coef[0] * lo[n];
    if (Channels == 2) {
        LONGLONG b = (LONGLONG)D->Coef[0] * lo[n + other];
        LONGLONG a1 = 0, b1 = 0;
        ULONG j = 0;
#pragma loop(no_vector)
        for (; j + 2 <= n; j += 2, lo += 2, hi -= 2, h -= 2) {
            LONGLONG c0 = h[0], c1 = h[-1];
            a += c0 * (lo[0] + hi[0]);
            b += c0 * (lo[other] + hi[other]);
            a1 += c1 * (lo[1] + hi[-1]);
            b1 += c1 * (lo[other + 1] + hi[other - 1]);
        }
        if (j < n) { a += (LONGLONG)h[0] * (lo[0] + hi[0]); b += (LONGLONG)h[0] * (lo[other] + hi[other]); }
        a += a1;
        Out[1] = (b + b1 + (1ll << 27)) >> 28;
    } else {
#pragma loop(no_vector)
        for (ULONG j = 0; j < n; ++j, ++lo, --hi, --h) a += (LONGLONG)*h * (lo[0] + hi[0]);
    }
    Out[0] = (a + (1ll << 27)) >> 28;
}

/* One polyphase output sample per channel: phase RsPhase over the window ending with the newest
 * input. |w| <= 2^33 and every phase's sum of |taps| is at most 2.75 * 2^28 (generator:
 * below 3.5 * 2^28), so |acc| < 2^63. */
static VOID RsOutputs(_In_ const PN_PCM_STATE *S, ULONG Channels, _Out_writes_(Channels) LONGLONG *Out)
{
    const SIZE_T other = 2 * PN_RS_MAX_TAPS;     /* channel 1's window, at a fixed distance from channel 0's */
    const PN_RS_TABLE *t = S->Rs;
    const ULONG taps = t->Taps;
    const LONG *c = t->Coef + (SIZE_T)S->RsPhase * taps;
    const LONGLONG *w = &S->RsHistory[0][S->RsPos];   /* w[0] oldest .. w[Taps - 1] newest */
    LONGLONG a = 0;
    if (Channels == 2) {
        LONGLONG b = 0;
        LONGLONG a1 = 0, b1 = 0;
        ULONG j = 0;
#pragma loop(no_vector)
        for (; j + 2 <= taps; j += 2) {
            LONGLONG c0 = c[j], c1 = c[j + 1];
            a += c0 * w[j];
            b += c0 * w[j + other];
            a1 += c1 * w[j + 1];
            b1 += c1 * w[j + 1 + other];
        }
        if (j < taps) { a += (LONGLONG)c[j] * w[j]; b += (LONGLONG)c[j] * w[j + other]; }
        a += a1;
        Out[1] = (b + b1 + (1ll << 27)) >> 28;
    } else {
#pragma loop(no_vector)
        for (ULONG j = 0; j < taps; ++j) a += (LONGLONG)c[j] * w[j];
    }
    Out[0] = (a + (1ll << 27)) >> 28;
}

/* Triangular (TPDF) dither of +-1 LSB of S16, in S32 units: two uniform 16-bit values. */
static LONG DitherS16(_Inout_ PN_PCM_STATE *S)
{
    ULONG x = S->DitherSeed ? S->DitherSeed : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    S->DitherSeed = x;
    return (LONG)(x & 0xffff) + (LONG)(x >> 16) - 65535;
}

/* IEEE binary32 bits of v / 2^31, rounded to nearest even exactly as a float division
 * would, with integer arithmetic only (no floating point in the kernel). Any 64-bit v: float
 * over full scale (fades keep it, finding #5/#9) as well as S32 values. */
static ULONG S64ToF32Bits(LONGLONG v)
{
    if (!v) return 0;
    ULONG sign = v < 0 ? 0x80000000u : 0;
    ULONGLONG mag = v < 0 ? 0ull - (ULONGLONG)v : (ULONGLONG)v;   /* 1 .. 2^63 */
    ULONG msb = 63;
    while (!(mag & (1ull << msb))) --msb;
    ULONG exponent = msb + 96;   /* 2^msb / 2^31, biased by 127 */
    ULONGLONG mantissa;
    if (msb <= 23) {
        mantissa = mag << (23 - msb);
    } else {
        ULONG shift = msb - 23;
        ULONGLONG rest = mag & ((1ull << shift) - 1), half = 1ull << (shift - 1);
        mantissa = mag >> shift;
        if (rest > half || (rest == half && (mantissa & 1))) ++mantissa;
        if (mantissa == (1ull << 24)) { mantissa >>= 1; ++exponent; }
    }
    return sign | (exponent << 23) | ((ULONG)mantissa & 0x7fffff);
}

static ULONG S32ToF32Bits(LONG v)
{
    return S64ToF32Bits(v);
}

/* Round24: a converted stream rounds S24 output to nearest; the native path truncates, as it
 * always did (bit-exact with 0.1.0.21). */
static VOID StoreSample(PUCHAR d, ULONG Target, LONGLONG Wide, LONG Dither, BOOLEAN Round24)
{
    LONG v = SaturateS32(Wide);   /* the one saturation of the path */
    if (Target == PNA2DP_FMT_S16) {
        LONGLONG rounded = (LONGLONG)v + 0x8000 + Dither;
        rounded = PN_MIN(rounded, (LONGLONG)2147483647);
        rounded = PN_MAX(rounded, (LONGLONG)(-2147483647 - 1));
        SHORT sample = (SHORT)(rounded >> 16);
        RtlCopyMemory(d, &sample, 2);
    } else if (Target == PNA2DP_FMT_S24) {
        ULONG sample = Round24 ? (ULONG)(LONG)(PN_MIN((LONGLONG)v + 0x80, (LONGLONG)2147483647) >> 8) : (ULONG)v >> 8;
        RtlCopyMemory(d, &sample, 3);
    } else if (Target == PNA2DP_FMT_F32) {
        ULONG sample = S32ToF32Bits(v);
        RtlCopyMemory(d, &sample, 4);
    } else {
        RtlCopyMemory(d, &v, 4);
    }
}

/* Review #8: digital silence stays digital silence. TRUE when the block is all zero and
 * nothing else feeds the output (no crossfade pending, empty filter histories). */
static BOOLEAN PcmSilentBlock(const PN_PCM_STATE *State, const UCHAR *In, ULONG Bytes, BOOLEAN Decimate, ULONG Channels,
                              BOOLEAN FadePending)
{
    if (FadePending) return FALSE;
    for (ULONG i = 0; i < Bytes; ++i) if (In[i]) return FALSE;
    if (Decimate) {
        for (ULONG c = 0; c < Channels; ++c)
            for (ULONG i = 0; i < 2 * PN_DEC2_MAX_TAPS; ++i) if (State->DecHistory[c][i]) return FALSE;
    }
    if (State && State->Rs) {
        for (ULONG c = 0; c < Channels; ++c)
            for (ULONG i = 0; i < 2 * PN_RS_MAX_TAPS; ++i) if (State->RsHistory[c][i]) return FALSE;
    }
    return TRUE;
}

/* Polyphase outputs that N2 more inputs complete from S (the one due already included). */
static ULONG RsOutputsFrom(_In_ const PN_PCM_STATE *S, ULONG N2)
{
    const PN_RS_TABLE *t = S->Rs;
    if (N2 < S->RsPending) return 0;
    ULONGLONG span = ((ULONGLONG)N2 - S->RsPending + 1) * t->L - 1 - S->RsPhase;
    ULONGLONG n = span / t->M + 1;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (ULONG)n;
}

ULONG PnPcmInputFramesFor(const PN_PCM_STATE *State, ULONG OutFrames)
{
    if (!State || !OutFrames) return OutFrames;
    /* the 2:1 stage's inputs: the rest of the current pair, then whole pairs */
    ULONGLONG n = OutFrames;
    if (State->Decimate == 2) n = 2 * n - (State->DecPhase & 1);
    /* the polyphase stage's inputs for n more outputs (the one due already counted) */
    if (State->Rs) n = State->RsPending + ((ULONGLONG)State->RsPhase + (n - 1) * State->Rs->M) / State->Rs->L;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (ULONG)n;
}

ULONG PnPcmMaxInputFrames(const PN_PCM_RATE_PLAN *Plan, ULONG OutFrames)
{
    if (!Plan || !OutFrames) return OutFrames;
    ULONGLONG n = Plan->Decimate == 2 ? 2ull * OutFrames : OutFrames;
    /* any state: floor((phase before the last output + n * M) / L) polyphase inputs at most */
    if (Plan->Rs) n = ((ULONGLONG)Plan->Rs->L - 1 + n * Plan->Rs->M) / Plan->Rs->L;
    return n > 0xFFFFFFFFull ? 0xFFFFFFFFu : (ULONG)n;
}

/* PnConvertPcmEx with a polyphase stage (State->Rs; the input checked by the caller). Every input
 * frame takes the native path's steps (non-finite concealment, the mix to mono, the trim crossfade
 * in input frames), then the polyphase window; a polyphase output is computed once its last input
 * is in and, in a cascade, feeds the 2:1 stage (limited to +-2^33), whose output is the call's
 * output once a pair is in; each output takes the gain ramp across this call's outputs and one
 * rounding at the store (TPDF dither for S16, Round24 for S24). A polyphase output due after the
 * call's last output frame waits for the next call (the same result: its window is complete),
 * unless more of this call's input must enter the window first; then it is computed and opens
 * the next pair. */
static ULONG ConvertResampled(_Inout_ PN_PCM_STATE *S, const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                              ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                              ULONG Target, ULONG TargetChannels, ULONG GainQ15)
{
    const PN_RS_TABLE *t = S->Rs;
    ULONG inBps = ContainerBits / 8;
    ULONG outBps = Target == PNA2DP_FMT_S16 ? 2 : Target == PNA2DP_FMT_S24 ? 3 : 4;
    ULONG inFrames = InBytes / (Channels * inBps), outCap = OutCap / (TargetChannels * outBps);
    BOOLEAN decimate = S->Decimate == 2;
    const PN_DEC2_TABLE *dec = DecTable(S);
    ULONG mids = RsOutputsFrom(S, inFrames);   /* polyphase outputs the input completes */
    ULONG possible = decimate ? (ULONG)(((ULONGLONG)(S->DecPhase & 1) + mids) / 2) : mids;
    ULONG frames = PN_MIN(possible, outCap);
    /* all input when every output it completes fits (later frames wait in the filter history),
     * else exactly the input of the frames written */
    ULONG used = frames == possible ? inFrames : PnPcmInputFramesFor(S, frames);
    LONG g0 = (LONG)GainQ15, g1 = (LONG)GainQ15;
    ULONG fadeFrames = 0, fadePos = 0;
    if (frames) {
        if (S->GainValid && S->GainQ15 <= 32768) g0 = (LONG)S->GainQ15;
        S->GainValid = TRUE;
        S->GainQ15 = GainQ15;
    }
    if (used && S->FadeOut && S->FadePos < S->FadeFrames) { fadeFrames = S->FadeFrames; fadePos = S->FadePos; }
    BOOLEAN rampIn = S->RampIn && used;
    /* a converted sample is never a 16-bit value: dithered unless nothing but silence is in */
    BOOLEAN dither = S->Dither && Target == PNA2DP_FMT_S16;
    if (dither && PcmSilentBlock(S, In, used * Channels * inBps, decimate, TargetChannels, fadeFrames != 0)) dither = FALSE;
    ULONG o = 0;   /* output frame */
    for (ULONG f = 0;; ++f) {
        while (S->RsPending == 0 && (o < frames || (decimate && f < used))) {
            LONGLONG y[2];
            RsOutputs(S, TargetChannels, y);
            /* (phase + M) / L and its remainder without a division (the quotient is 0..2) */
            S->RsPhase += t->M;
            S->RsPending = 0;
            while (S->RsPhase >= t->L) { S->RsPhase -= t->L; S->RsPending++; }
            if (decimate) {
                /* into the 2:1 stage, limited to +-2^33 (the overflow bound) */
                LONGLONG *h0 = &S->DecHistory[0][S->DecPos], *h1 = &S->DecHistory[1][S->DecPos];
                h0[0] = h0[dec->Taps] = ClampFloatLimit(y[0]);
                if (TargetChannels == 2) h1[0] = h1[dec->Taps] = ClampFloatLimit(y[1]);
                S->DecPos = S->DecPos + 1 == dec->Taps ? 0 : S->DecPos + 1;
                if (++S->DecPhase < 2) continue;
                S->DecPhase = 0;
                DecOutputs(S, dec, TargetChannels, y);
            }
            LONG gain = g0 == g1 ? g1 : g0 + (LONG)(((LONGLONG)(g1 - g0) * (LONG)(o + 1)) / (LONG)frames);
            for (ULONG c = 0; c < TargetChannels; ++c) {
                LONGLONG s = (y[c] * gain) / 32768;
                StoreSample(Out + (o * TargetChannels + c) * outBps, Target, s, dither && gain ? DitherS16(S) : 0, S->Round24);
            }
            ++o;
        }
        if (f >= used) break;
        ULONG k = fadePos + f;   /* position in the trim crossfade (input frames) */
        LONGLONG v[2] = { 0, 0 }, src[2] = { 0, 0 };
        for (ULONG sc = 0; sc < Channels; ++sc) {
            const UCHAR *s = In + (f * Channels + sc) * inBps;
            src[sc] = ReadSample(s, inBps, ValidBits, IsFloat);
            if (IsFloat) src[sc] = ConcealNonFinite(S, sc, s, src[sc]);
        }
        for (ULONG c = 0; c < TargetChannels; ++c) {
            v[c] = Channels == 2 && TargetChannels == 1 ? (src[0] + src[1]) / 2 : src[PN_MIN(c, Channels - 1)];
            if (k < fadeFrames) {
                LONGLONG fo = ReadFrameSample(S->FadeOut, k, c, Channels, TargetChannels, inBps, ValidBits, IsFloat);
                v[c] = fo + ((v[c] - fo) * (LONGLONG)(k + 1)) / (LONGLONG)(fadeFrames + 1);
            }
            if (rampIn) v[c] = (v[c] * (LONGLONG)(f + 1)) / (LONGLONG)(used + 1);   /* mid-programme start, before the filters */
        }
        for (ULONG c = 0; c < TargetChannels; ++c)
            S->RsHistory[c][S->RsPos] = S->RsHistory[c][S->RsPos + t->Taps] = v[c];
        S->RsPos = S->RsPos + 1 == t->Taps ? 0 : S->RsPos + 1;
        if (S->RsPending) S->RsPending--;   /* never 0 here: a due output was written above or lies past `used` */
    }
    if (rampIn) S->RampIn = FALSE;
    if (fadeFrames) {
        S->FadePos = fadePos + used;
        if (S->FadePos >= fadeFrames) { S->FadeOut = NULL; S->FadeFrames = S->FadePos = 0; }
    }
    return o * TargetChannels * outBps;
}

ULONG PnConvertPcmEx(PN_PCM_STATE *State, const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                     ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                     ULONG Target, ULONG TargetChannels, ULONG GainQ15)
{
    if (!In || !Out || !Channels || Channels > 2 || !TargetChannels || TargetChannels > 2 ||
        (ContainerBits != 16 && ContainerBits != 24 && ContainerBits != 32) ||
        (IsFloat && ContainerBits != 32) || !ValidBits || ValidBits > ContainerBits || GainQ15 > 32768 ||
        (Target != PNA2DP_FMT_S16 && Target != PNA2DP_FMT_S24 && Target != PNA2DP_FMT_S32 && Target != PNA2DP_FMT_F32)) return 0;
    /* a polyphase stage has its own loop; the native and 2:1 paths below are unchanged */
    if (State && State->Rs) return ConvertResampled(State, In, InBytes, Channels, ContainerBits, ValidBits, IsFloat, Out, OutCap,
                                                    Target, TargetChannels, GainQ15);
    ULONG inBps = ContainerBits / 8;
    ULONG outBps = Target == PNA2DP_FMT_S16 ? 2 : Target == PNA2DP_FMT_S24 ? 3 : 4;
    ULONG inFrames = InBytes / (Channels * inBps), outCap = OutCap / (TargetChannels * outBps);
    BOOLEAN decimate = State && State->Decimate == 2;
    const PN_DEC2_TABLE *dec = decimate ? DecTable(State) : NULL;
    ULONG frames, used;   /* output frames written, input frames consumed */
    if (!decimate) {
        frames = used = PN_MIN(inFrames, outCap);
    } else {
        ULONG phase = State->DecPhase & 1, possible = (phase + inFrames) / 2;
        frames = PN_MIN(possible, outCap);
        /* all input when it fits (a trailing odd frame waits in the filter), else exactly
         * the input of the frames written */
        used = frames == possible ? inFrames : frames * 2 - phase;
    }
    /* gain ramps linearly from the previous block's gain to GainQ15 across this block */
    LONG g0 = (LONG)GainQ15, g1 = (LONG)GainQ15;
    ULONG fadeFrames = 0, fadePos = 0;
    if (State && frames) {
        if (State->GainValid && State->GainQ15 <= 32768) g0 = (LONG)State->GainQ15;
        State->GainValid = TRUE;
        State->GainQ15 = GainQ15;
    }
    if (State && used && State->FadeOut && State->FadePos < State->FadeFrames) { fadeFrames = State->FadeFrames; fadePos = State->FadePos; }
    BOOLEAN rampIn = State && State->RampIn && used;   /* the 2:1 stage's mid-programme start (never native) */
    /* Requantising to 16 bits is dithered unless every sample already is a 16-bit value. */
    BOOLEAN dither = State && State->Dither && Target == PNA2DP_FMT_S16 &&
        (g0 != 32768 || g1 != 32768 || IsFloat || ValidBits > 16 || decimate || fadeFrames);
    if (dither && PcmSilentBlock(State, In, used * Channels * inBps, decimate, TargetChannels, fadeFrames != 0)) dither = FALSE;
    ULONG o = 0;   /* output frame */
    for (ULONG f = 0; f < used; ++f) {
        ULONG k = fadePos + f;   /* position in the trim crossfade (input frames) */
        LONGLONG v[2] = { 0, 0 };
        /* the input frame once per input channel (a non-finite float sample concealed), then
         * mixed down when stereo becomes mono */
        LONGLONG src[2] = { 0, 0 };
        for (ULONG sc = 0; sc < Channels; ++sc) {
            const UCHAR *s = In + (f * Channels + sc) * inBps;
            src[sc] = ReadSample(s, inBps, ValidBits, IsFloat);
            if (IsFloat && State) src[sc] = ConcealNonFinite(State, sc, s, src[sc]);
        }
        for (ULONG c = 0; c < TargetChannels; ++c) {
            v[c] = Channels == 2 && TargetChannels == 1 ? (src[0] + src[1]) / 2 : src[PN_MIN(c, Channels - 1)];
            if (k < fadeFrames) {
                /* from the audio that would have followed the cut to the kept audio */
                LONGLONG fo = ReadFrameSample(State->FadeOut, k, c, Channels, TargetChannels, inBps, ValidBits, IsFloat);
                v[c] = fo + ((v[c] - fo) * (LONGLONG)(k + 1)) / (LONGLONG)(fadeFrames + 1);
            }
            if (rampIn) v[c] = (v[c] * (LONGLONG)(f + 1)) / (LONGLONG)(used + 1);
        }
        if (decimate) {
            for (ULONG c = 0; c < TargetChannels; ++c)
                State->DecHistory[c][State->DecPos] = State->DecHistory[c][State->DecPos + dec->Taps] = v[c];
            State->DecPos = State->DecPos + 1 == dec->Taps ? 0 : State->DecPos + 1;
            if (++State->DecPhase < 2) continue;
            State->DecPhase = 0;
            DecOutputs(State, dec, TargetChannels, v);
        }
        LONG gain = g0 == g1 ? g1 : g0 + (LONG)(((LONGLONG)(g1 - g0) * (LONG)(o + 1)) / (LONG)frames);
        for (ULONG c = 0; c < TargetChannels; ++c) {
            LONGLONG s = (v[c] * gain) / 32768;
            StoreSample(Out + (o * TargetChannels + c) * outBps, Target, s, dither && gain ? DitherS16(State) : 0, State && State->Round24);
        }
        ++o;
    }
    if (rampIn) State->RampIn = FALSE;
    if (fadeFrames) {
        State->FadePos = fadePos + used;
        if (State->FadePos >= fadeFrames) { State->FadeOut = NULL; State->FadeFrames = State->FadePos = 0; }
    }
    return frames * TargetChannels * outBps;
}

BOOLEAN PnPcmRatePlan(ULONG EngineRate, ULONG CodecRate, PN_PCM_RATE_PLAN *Plan)
{
    RtlZeroMemory(Plan, sizeof(*Plan));
    ULONG decimate = 1;
    const PN_RS_TABLE *rs = NULL;
    const PN_DEC2_TABLE *dec = NULL;
    if (!EngineRate || !CodecRate) return FALSE;
    if (EngineRate == CodecRate) {
        /* native */
    } else if ((EngineRate == 96000 && CodecRate == 48000) || (EngineRate == 88200 && CodecRate == 44100)) {
        decimate = 2;
    } else if (EngineRate == 44100 && CodecRate == 48000) {
        rs = &g_PnRs44to48;
    } else if (EngineRate == 48000 && CodecRate == 44100) {
        rs = &g_PnRs48to44;
    } else if (EngineRate == 88200 && CodecRate == 48000) {
        decimate = 2; rs = &g_PnRs88to96;   /* into 96 kHz, then 2:1 */
    } else if (EngineRate == 96000 && CodecRate == 44100) {
        decimate = 2; rs = &g_PnRs96to88;   /* into 88.2 kHz, then 2:1 */
    } else {
        return FALSE;   /* 32 / 16 kHz engines, 176.4 / 192 kHz, conversion up to 88.2 / 96 kHz */
    }
    /* the 2:1 FIR of its input rate (the engine's, or the polyphase stage's output in a cascade)
     * stops from the final Nyquist frequency on its own (the alias-free band plan, pcm.h): 24 kHz
     * from 96 kHz, 22.05 kHz from 88.2 kHz */
    if (decimate == 2) dec = 2 * CodecRate == 96000 ? &g_PnDec96 : &g_PnDec88;
    Plan->EngineRate = EngineRate;
    Plan->CodecRate = CodecRate;
    Plan->Decimate = decimate;
    Plan->Dec = dec;
    Plan->Rs = rs;
    /* a polyphase stage holds CenterTwice / (2 L) engine frames (its group delay); the 2:1 stage
     * Center of its input frames (its group delay and the wait for a pair's second frame): engine
     * frames, or in a cascade the polyphase stage's outputs, M / L engine frames each; rounded */
    if (!rs) Plan->DelayEngineFrames = dec ? dec->Center : 0;
    else Plan->DelayEngineFrames = (rs->CenterTwice + (dec ? 2 * dec->Center * rs->M : 0) + rs->L) / (2 * rs->L);
    return TRUE;
}

VOID PnPcmStateResetRate(PN_PCM_STATE *State, const PN_PCM_RATE_PLAN *Plan, BOOLEAN FadeIn)
{
    RtlZeroMemory(State, sizeof(*State));
    State->Decimate = Plan && Plan->Decimate == 2 ? 2 : 1;
    State->Dec = State->Decimate == 2 ? Plan->Dec : NULL;
    State->Rs = Plan ? Plan->Rs : NULL;
    State->RsPending = 1;   /* polyphase output 0 needs input 0 */
    State->Round24 = Plan && Plan->EngineRate != Plan->CodecRate;
    if (FadeIn && State->Round24) {
        State->RampIn = TRUE;   /* converted: the first block's input ramps in, before the filters */
    } else if (FadeIn) {
        /* the first block ramps from silence to the requested gain */
        State->GainValid = TRUE;
        State->GainQ15 = 0;
    }
}

VOID PnPcmStateReset(PN_PCM_STATE *State, ULONG Decimate, BOOLEAN FadeIn)
{
    RtlZeroMemory(State, sizeof(*State));
    State->Decimate = Decimate;
    State->RsPending = 1;
    if (FadeIn) {
        /* the first block ramps from silence to the requested gain */
        State->GainValid = TRUE;
        State->GainQ15 = 0;
    }
}

/* A sample in its own format from S32 units (integer: the valid bits only, saturated; float:
 * unsaturated, a value over full scale stays over it as everywhere else on the path). */
static VOID WriteNativeSample(PUCHAR d, ULONG Bytes, ULONG Valid, BOOLEAN IsFloat, LONGLONG v)
{
    ULONG bits;
    if (IsFloat) bits = S64ToF32Bits(v);
    else {
        bits = (ULONG)SaturateS32(v);
        if (Valid < Bytes * 8) bits &= ~((1u << (32 - Valid)) - 1);
        bits >>= 32 - Bytes * 8;
    }
    RtlCopyMemory(d, &bits, Bytes);
}

VOID PnPcmRamp(PUCHAR Pcm, ULONG Frames, ULONG Channels, ULONG ContainerBits, ULONG ValidBits, BOOLEAN IsFloat,
               ULONG RampPos, ULONG RampLen, BOOLEAN Up)
{
    if (!Pcm || !Channels || Channels > 2 || (ContainerBits != 16 && ContainerBits != 24 && ContainerBits != 32) ||
        (IsFloat && ContainerBits != 32) || !ValidBits || ValidBits > ContainerBits) return;
    /* raised cosine sin^2(pi/2 * x) at x = i / 128, Q15: no slope step at either end, so a
     * fade adds far less splatter than a linear one of the same length */
    static const USHORT shape[129] = {
        0, 5, 20, 44, 79, 123, 177, 241, 315, 398, 491, 593, 705, 827, 958, 1098,
        1247, 1406, 1573, 1749, 1935, 2128, 2331, 2542, 2761, 2989, 3224, 3468, 3719, 3978, 4244, 4518,
        4799, 5087, 5381, 5682, 5990, 6304, 6624, 6950, 7282, 7619, 7961, 8308, 8661, 9018, 9379, 9745,
        10114, 10487, 10864, 11245, 11628, 12014, 12403, 12794, 13188, 13583, 13980, 14378, 14778, 15179, 15580, 15982,
        16384, 16786, 17188, 17589, 17990, 18390, 18788, 19185, 19580, 19974, 20365, 20754, 21140, 21523, 21904, 22281,
        22654, 23023, 23389, 23750, 24107, 24460, 24807, 25149, 25486, 25818, 26144, 26464, 26778, 27086, 27387, 27681,
        27969, 28250, 28524, 28790, 29049, 29300, 29544, 29779, 30007, 30226, 30437, 30640, 30833, 31019, 31195, 31362,
        31521, 31670, 31810, 31941, 32063, 32175, 32277, 32370, 32453, 32527, 32591, 32645, 32689, 32724, 32748, 32763,
        32768
    };
    ULONG bytes = ContainerBits / 8;
    for (ULONG f = 0; f < Frames; ++f) {
        ULONG k = RampPos + f;
        if (Up && k >= RampLen) break;   /* past the fade-in: unchanged */
        LONG g = 0;   /* Q15 */
        if (k < RampLen) {
            /* x = (k + 1) / (RampLen + 1) up, (RampLen - k) / (RampLen + 1) down; the shape is
             * interpolated between its 129 points */
            ULONGLONG num = (ULONGLONG)(Up ? k + 1 : RampLen - k) * 128u;
            ULONG idx = (ULONG)(num / (RampLen + 1));
            ULONGLONG frac = (num % (RampLen + 1)) * 65536u / (RampLen + 1);
            g = shape[idx] + (LONG)(((LONGLONG)(idx < 128 ? shape[idx + 1] - shape[idx] : 0) * (LONGLONG)frac) >> 16);
        }
        for (ULONG c = 0; c < Channels; ++c) {
            PUCHAR s = Pcm + (f * Channels + c) * bytes;
            /* 64-bit and unsaturated (findings #5/#9): float over full scale (|v| <= 2^33) keeps
             * its value through the fade as it does through the gain, so a fade edge in hot float
             * programme at reduced volume is no step; integer samples cannot exceed S32 */
            LONGLONG v = ReadSample(s, bytes, ValidBits, IsFloat);
            WriteNativeSample(s, bytes, ValidBits, IsFloat, (v * g) / 32768);
        }
    }
}

ULONG PnConvertPcm(const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                   ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                   ULONG Target, ULONG TargetChannels, ULONG GainQ15)
{
    return PnConvertPcmEx(NULL, In, InBytes, Channels, ContainerBits, ValidBits, IsFloat, Out, OutCap, Target, TargetChannels, GainQ15);
}
