/* SPDX-License-Identifier: MIT */
#include "codec_core.h"
#include "pcm.h"

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
 * a crossfade and the decimator's overshoot keep their value until the gain has been applied,
 * and the result saturates once when it is stored. Float input is held to +-4.0 (+12 dBFS):
 * the half-band filter's 64-bit accumulator then cannot overflow. */
#define PN_PCM_FLOAT_LIMIT   (1ll << 33)

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

/* Kaiser-windowed (beta 8) half-band design, quantised to Q28 with the DC sum made exact:
 * 2^27 (centre) + 2 * sum = 2^28. */
const LONG g_PnHalfbandQ28[PN_HB_HALF] = {
    85323658, -28115438, 16484339, -11372405, 8442743, -6514558,
    5135043, -4093707, 3279516, -2628248, 2099946, -1668148,
    1314319, -1024787, 788980, -598382, 445898, -325467,
    231827, -160362, 107023, -68265, 41009, -22618,
    10867, -3919
};

/* One filtered output sample from the window ending with the newest input. Not saturated:
 * the overshoot of a full-scale input survives a gain below unity (finding #3). */
static LONGLONG HalfbandOutput(_In_ const PN_PCM_STATE *S, ULONG Channel)
{
    const LONGLONG *w = &S->HbHistory[Channel][S->HbPos];   /* w[0] oldest .. w[PN_HB_TAPS - 1] newest */
    /* |w| <= 2^33, sum of |taps| = 1.843 * 2^28: |sum| < 2^33 * 2^28 * 1.85 < 2^62, no overflow */
    LONGLONG acc = w[PN_HB_CENTER] * (1ll << 27);
    for (ULONG j = 0; j < PN_HB_HALF; ++j)
        acc += (LONGLONG)g_PnHalfbandQ28[j] * (w[PN_HB_CENTER - 1 - 2 * j] + w[PN_HB_CENTER + 1 + 2 * j]);
    return (acc + (1ll << 27)) >> 28;
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

static VOID StoreSample(PUCHAR d, ULONG Target, LONGLONG Wide, LONG Dither)
{
    LONG v = SaturateS32(Wide);   /* the one saturation of the path */
    if (Target == PNA2DP_FMT_S16) {
        LONGLONG rounded = (LONGLONG)v + 0x8000 + Dither;
        rounded = PN_MIN(rounded, (LONGLONG)2147483647);
        rounded = PN_MAX(rounded, (LONGLONG)(-2147483647 - 1));
        SHORT sample = (SHORT)(rounded >> 16);
        RtlCopyMemory(d, &sample, 2);
    } else if (Target == PNA2DP_FMT_S24) {
        ULONG sample = (ULONG)v >> 8;
        RtlCopyMemory(d, &sample, 3);
    } else if (Target == PNA2DP_FMT_F32) {
        ULONG sample = S32ToF32Bits(v);
        RtlCopyMemory(d, &sample, 4);
    } else {
        RtlCopyMemory(d, &v, 4);
    }
}

/* Review #8: digital silence stays digital silence. TRUE when the block is all zero and
 * nothing else feeds the output (no crossfade pending, an empty decimator history). */
static BOOLEAN PcmSilentBlock(const PN_PCM_STATE *State, const UCHAR *In, ULONG Bytes, BOOLEAN Decimate, ULONG Channels,
                              BOOLEAN FadePending)
{
    if (FadePending) return FALSE;
    for (ULONG i = 0; i < Bytes; ++i) if (In[i]) return FALSE;
    if (Decimate) {
        for (ULONG c = 0; c < Channels; ++c)
            for (ULONG i = 0; i < 2 * PN_HB_TAPS; ++i) if (State->HbHistory[c][i]) return FALSE;
    }
    return TRUE;
}

ULONG PnConvertPcmEx(PN_PCM_STATE *State, const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                     ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                     ULONG Target, ULONG TargetChannels, ULONG GainQ15)
{
    if (!In || !Out || !Channels || Channels > 2 || !TargetChannels || TargetChannels > 2 ||
        (ContainerBits != 16 && ContainerBits != 24 && ContainerBits != 32) ||
        (IsFloat && ContainerBits != 32) || !ValidBits || ValidBits > ContainerBits || GainQ15 > 32768 ||
        (Target != PNA2DP_FMT_S16 && Target != PNA2DP_FMT_S24 && Target != PNA2DP_FMT_S32 && Target != PNA2DP_FMT_F32)) return 0;
    ULONG inBps = ContainerBits / 8;
    ULONG outBps = Target == PNA2DP_FMT_S16 ? 2 : Target == PNA2DP_FMT_S24 ? 3 : 4;
    ULONG inFrames = InBytes / (Channels * inBps), outCap = OutCap / (TargetChannels * outBps);
    BOOLEAN decimate = State && State->Decimate == 2;
    ULONG frames, used;   /* output frames written, input frames consumed */
    if (!decimate) {
        frames = used = PN_MIN(inFrames, outCap);
    } else {
        ULONG phase = State->HbPhase & 1, possible = (phase + inFrames) / 2;
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
        }
        if (decimate) {
            for (ULONG c = 0; c < TargetChannels; ++c)
                State->HbHistory[c][State->HbPos] = State->HbHistory[c][State->HbPos + PN_HB_TAPS] = v[c];
            State->HbPos = (State->HbPos + 1) % PN_HB_TAPS;
            if (++State->HbPhase < 2) continue;
            State->HbPhase = 0;
            for (ULONG c = 0; c < TargetChannels; ++c) v[c] = HalfbandOutput(State, c);
        }
        LONG gain = g0 == g1 ? g1 : g0 + (LONG)(((LONGLONG)(g1 - g0) * (LONG)(o + 1)) / (LONG)frames);
        for (ULONG c = 0; c < TargetChannels; ++c) {
            LONGLONG s = (v[c] * gain) / 32768;
            StoreSample(Out + (o * TargetChannels + c) * outBps, Target, s, dither && gain ? DitherS16(State) : 0);
        }
        ++o;
    }
    if (fadeFrames) {
        State->FadePos = fadePos + used;
        if (State->FadePos >= fadeFrames) { State->FadeOut = NULL; State->FadeFrames = State->FadePos = 0; }
    }
    return frames * TargetChannels * outBps;
}

VOID PnPcmStateReset(PN_PCM_STATE *State, ULONG Decimate, BOOLEAN FadeIn)
{
    RtlZeroMemory(State, sizeof(*State));
    State->Decimate = Decimate;
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
