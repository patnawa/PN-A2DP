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

static LONG ReadSample(const UCHAR *s, ULONG bytes, ULONG valid, BOOLEAN floating)
{
    ULONG bits = 0;
    RtlCopyMemory(&bits, s, bytes);
    if (floating) {
        // Decode IEEE binary32 directly: +1.0f cannot be converted through float * INT_MAX
        // because INT_MAX rounds to 2^31 in binary32. NaN becomes silence.
        ULONG magnitude = bits & 0x7fffffff;
        if (magnitude > 0x7f800000) return 0;
        if (magnitude >= 0x3f800000) return (bits >> 31) ? (-2147483647L - 1) : 2147483647L;
        ULONG exponent = magnitude >> 23;
        if (exponent < 96) return 0; // magnitude below one S32 unit
        ULONGLONG mantissa = (magnitude & 0x7fffff) | 0x800000;
        ULONG value = exponent >= 119 ? (ULONG)(mantissa << (exponent - 119)) : (ULONG)(mantissa >> (119 - exponent));
        return (bits >> 31) ? -(LONG)value : (LONG)value;
    }
    bits <<= 32 - bytes * 8;
    if (valid < bytes * 8) bits &= ~((1u << (32 - valid)) - 1);
    return (LONG)bits;
}

ULONG PnConvertPcm(const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                   ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                   ULONG Target, ULONG TargetChannels, ULONG GainQ15)
{
    if (!In || !Out || !Channels || Channels > 2 || !TargetChannels || TargetChannels > 2 ||
        (ContainerBits != 16 && ContainerBits != 24 && ContainerBits != 32) ||
        (IsFloat && ContainerBits != 32) || !ValidBits || ValidBits > ContainerBits || GainQ15 > 32768 ||
        (Target != PNA2DP_FMT_S16 && Target != PNA2DP_FMT_S24 && Target != PNA2DP_FMT_S32 && Target != PNA2DP_FMT_F32)) return 0;
    ULONG inBps = ContainerBits / 8;
    ULONG outBps = Target == PNA2DP_FMT_S16 ? 2 : Target == PNA2DP_FMT_S24 ? 3 : 4;
    ULONG frames = PN_MIN(InBytes / (Channels * inBps), OutCap / (TargetChannels * outBps));
    for (ULONG f = 0; f < frames; ++f) {
        for (ULONG c = 0; c < TargetChannels; ++c) {
            const UCHAR *s = In + (f * Channels + PN_MIN(c, Channels - 1)) * inBps;
            LONG v = ReadSample(s, inBps, ValidBits, IsFloat);
            if (Channels == 2 && TargetChannels == 1)
                v = (LONG)(((LONGLONG)v + ReadSample(s + inBps, inBps, ValidBits, IsFloat)) / 2);
            v = (LONG)(((LONGLONG)v * GainQ15) / 32768);
            UCHAR *d = Out + (f * TargetChannels + c) * outBps;
            if (Target == PNA2DP_FMT_S16) {
                LONGLONG rounded = PN_MIN((LONGLONG)v + 0x8000, (LONGLONG)2147483647);
                SHORT sample = (SHORT)(rounded >> 16);
                RtlCopyMemory(d, &sample, 2);
            } else if (Target == PNA2DP_FMT_S24) {
                ULONG sample = (ULONG)v >> 8;
                RtlCopyMemory(d, &sample, 3);
            } else if (Target == PNA2DP_FMT_F32) {
                float sample = (float)v / 2147483648.0f;
                RtlCopyMemory(d, &sample, 4);
            } else {
                RtlCopyMemory(d, &v, 4);
            }
        }
    }
    return frames * TargetChannels * outBps;
}
