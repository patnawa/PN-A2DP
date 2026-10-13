/* Pure WaveRT format rules shared with core_test: the PCM formats the render pin offers
 * for a sink, how an engine format is matched against them, the sanity checks on
 * capabilities cached in the registry, and the coalescing of endpoint notifications.
 * Integer only (kernel code); needs a2dp_codec.h. The caller provides the locking.
 * SPDX-License-Identifier: MIT */
#pragma once

/* 6 rates x (24-in-32, float, 16-bit) */
#define PN_WAVE_MAX_FORMATS     18

typedef struct _PN_WAVE_FORMAT {
    ULONG   Rate;
    USHORT  Channels;
    USHORT  ContainerBits;
    USHORT  ValidBits;
    BOOLEAN IsFloat;
} PN_WAVE_FORMAT;

typedef struct _PN_WAVE_FORMAT_INPUT {
    const PNA2DP_CAPABILITIES *Caps;    /* live or cached sink capabilities, NULL = unknown */
    ULONG   EnabledCodecMask;           /* PNA2DP_CODEC_BIT() of the codecs the configuration allows */
    BOOLEAN ActiveValid;                /* a configuration is negotiated */
    ULONG   ActiveCodec;
    ULONG   ActiveRate;
    BOOLEAN ExposeAllRates;             /* PNA2DP_CFG_EXPOSE_ALL_RATES */
    BOOLEAN AllowFloat;                 /* the pin has an IEEE float data range (fixed at filter creation) */
} PN_WAVE_FORMAT_INPUT;

/* Codecs whose encoder takes more than 16 bits (A2dpSelectConfig: aptX family S24,
 * LDAC S32). Their sinks get 24-in-32 and float formats; SBC and AAC encode 16-bit. */
static inline BOOLEAN PnWaveCodecIsHiRes(ULONG Codec)
{
    return Codec == PNA2DP_CODEC_APTX || Codec == PNA2DP_CODEC_APTX_HD || Codec == PNA2DP_CODEC_APTX_LL || Codec == PNA2DP_CODEC_LDAC;
}

/* Rate mask of one remote SEP when it is an audio sink with parseable capabilities. */
static inline BOOLEAN PnWaveSinkSepRates(const PNA2DP_REMOTE_SEP *Sep, ULONG *RateMask)
{
    *RateMask = 0;
    if (!Sep->Codec || Sep->MediaType != 0 || !Sep->TsepIsSink || Sep->CodecInfoLength > sizeof(Sep->CodecInfo)) return FALSE;
    UCHAR mct = (Sep->Codec == PNA2DP_CODEC_SBC) ? A2DP_CODEC_SBC : (Sep->Codec == PNA2DP_CODEC_AAC) ? A2DP_CODEC_MPEG24_AAC : A2DP_CODEC_VENDOR;
    A2DP_CODEC_CAPS caps;
    if (!A2dpParseCaps(mct, Sep->CodecInfo, Sep->CodecInfoLength, &caps, NULL, NULL) || caps.Codec != Sep->Codec) return FALSE;
    *RateMask = caps.RateMask;
    return TRUE;
}

/* TRUE when a codec that encodes more than 16 bits could carry this stream: the
 * negotiated one, or (connected or not) any sink codec the configuration enables. */
static inline BOOLEAN PnWaveFormatsWantHiRes(const PN_WAVE_FORMAT_INPUT *In)
{
    if (In->ActiveValid && PnWaveCodecIsHiRes(In->ActiveCodec)) return TRUE;
    if (!In->Caps) return FALSE;
    for (ULONG i = 0; i < In->Caps->SepCount && i < PNA2DP_MAX_REMOTE_SEPS; i++) {
        const PNA2DP_REMOTE_SEP *s = &In->Caps->Seps[i];
        ULONG rates;
        if (s->Codec >= 32 || !(In->EnabledCodecMask & PNA2DP_CODEC_BIT(s->Codec)) || !PnWaveCodecIsHiRes(s->Codec)) continue;
        if (PnWaveSinkSepRates(s, &rates)) return TRUE;
    }
    return FALSE;
}

/* Appends a stereo entry (padding zeroed, so tables compare with memcmp). */
static inline VOID PnWaveAddFormat(PN_WAVE_FORMAT *Out, ULONG *N, ULONG Cap, ULONG Rate, USHORT ContainerBits, USHORT ValidBits, BOOLEAN IsFloat)
{
    if (*N >= Cap) return;
    PN_WAVE_FORMAT f;
    RtlZeroMemory(&f, sizeof(f));
    f.Rate = Rate; f.Channels = 2; f.ContainerBits = ContainerBits; f.ValidBits = ValidBits; f.IsFloat = IsFloat;
    Out[(*N)++] = f;
}

/* The render pin's format table. Rules:
 *  - 16-bit stereo at 48 and 44.1 kHz is always offered: every A2DP sink supports both
 *    for SBC (mandatory), so the stream can always fall back to SBC.
 *  - The negotiated rate is added; with EXPOSE_ALL_RATES every rate an enabled sink
 *    codec supports is added too (48/44.1/96/88.2/32/16 kHz).
 *  - When a hi-res codec could carry the stream, each rate gets 24-in-32 (first, so it is
 *    the default) and, if the pin has a float range, 32-bit IEEE float before 16-bit. The
 *    pump converts any of them to the encoder's input format, so SBC still works.
 *  - 48 kHz comes first unless the negotiated rate is another one (for example 96 kHz
 *    chosen by the sink): a connect or disconnect does not reorder the usual table, so
 *    Windows does not see a format change for nothing.
 * The table depends on the sink, the configuration and the negotiated rate only, so it is
 * stable while a sink comes and goes. Returns the number of entries written. */
static inline ULONG PnWaveBuildFormats(const PN_WAVE_FORMAT_INPUT *In, PN_WAVE_FORMAT *Out, ULONG Cap)
{
    static const ULONG rateOrder[] = { 48000, 44100, 96000, 88200, 32000, 16000 };
    ULONG allowed = PNA2DP_RATE_48000 | PNA2DP_RATE_44100;
    ULONG sinkRates = 0;
    if (In->Caps) {
        for (ULONG i = 0; i < In->Caps->SepCount && i < PNA2DP_MAX_REMOTE_SEPS; i++) {
            const PNA2DP_REMOTE_SEP *s = &In->Caps->Seps[i];
            ULONG rates;
            if (s->Codec >= 32 || !(In->EnabledCodecMask & PNA2DP_CODEC_BIT(s->Codec))) continue;
            if (PnWaveSinkSepRates(s, &rates)) sinkRates |= rates;
        }
    }
    if (In->ActiveValid) allowed |= A2dpHzToRateMask(In->ActiveRate);
    if (In->ExposeAllRates) allowed |= sinkRates;
    BOOLEAN hiRes = PnWaveFormatsWantHiRes(In);
    ULONG firstRate = 48000;
    if (In->ActiveValid && In->ActiveRate != 44100 && (allowed & A2dpHzToRateMask(In->ActiveRate))) {
        for (ULONG i = 0; i < sizeof(rateOrder) / sizeof(rateOrder[0]); i++) if (rateOrder[i] == In->ActiveRate) firstRate = In->ActiveRate;
    }
    ULONG n = 0;
    for (ULONG pass = 0; pass < 2; pass++) {
        for (ULONG i = 0; i < sizeof(rateOrder) / sizeof(rateOrder[0]); i++) {
            ULONG r = rateOrder[i];
            if (!(allowed & A2dpHzToRateMask(r)) || (pass == 0) != (r == firstRate)) continue;
            if (hiRes) PnWaveAddFormat(Out, &n, Cap, r, 32, 24, FALSE);
            if (hiRes && In->AllowFloat) PnWaveAddFormat(Out, &n, Cap, r, 32, 32, TRUE);
            PnWaveAddFormat(Out, &n, Cap, r, 16, 16, FALSE);
        }
    }
    return n;
}

/* Sample representation of a proposed format: the KS subformat and the WAVEFORMATEX tag
 * (and for WAVE_FORMAT_EXTENSIBLE its SubFormat) must agree. */
#define PN_WAVE_SUB_PCM         0
#define PN_WAVE_SUB_FLOAT       1
#define PN_WAVE_SUB_WAVEFORMATEX 2      /* KSDATAFORMAT_SUBTYPE_WAVEFORMATEX: the tag decides */
#define PN_WAVE_SUB_OTHER       3
#define PN_WAVE_TAG_PCM         0x0001
#define PN_WAVE_TAG_IEEE_FLOAT  0x0003
#define PN_WAVE_TAG_EXTENSIBLE  0xFFFE

/* Returns 0 for integer PCM, 1 for IEEE float, -1 to reject. ExtSub is the extensible
 * SubFormat (PN_WAVE_SUB_PCM/FLOAT/OTHER), used only with the extensible tag. */
static inline int PnWaveFormatKind(ULONG KsSub, USHORT Tag, ULONG ExtSub)
{
    int kind;
    if (Tag == PN_WAVE_TAG_EXTENSIBLE) kind = ExtSub == PN_WAVE_SUB_PCM ? 0 : ExtSub == PN_WAVE_SUB_FLOAT ? 1 : -1;
    else if (Tag == PN_WAVE_TAG_PCM) kind = 0;
    else if (Tag == PN_WAVE_TAG_IEEE_FLOAT) kind = 1;
    else kind = -1;
    if (kind < 0) return -1;
    if (KsSub == PN_WAVE_SUB_PCM) return kind == 0 ? 0 : -1;
    if (KsSub == PN_WAVE_SUB_FLOAT) return kind == 1 ? 1 : -1;
    if (KsSub == PN_WAVE_SUB_WAVEFORMATEX) return kind;
    return -1;
}

/* Index of the table entry that matches exactly, or -1. Every field later used for
 * arithmetic (block align, byte rate) must be consistent with the entry. */
static inline LONG PnWaveFindFormat(const PN_WAVE_FORMAT *T, ULONG Count, ULONG Rate, ULONG Channels, ULONG ContainerBits, ULONG ValidBits,
                                    ULONG BlockAlign, ULONG AvgBytesPerSec, BOOLEAN IsFloat)
{
    for (ULONG i = 0; i < Count; i++) {
        const PN_WAVE_FORMAT *f = &T[i];
        ULONG align = (ULONG)f->Channels * f->ContainerBits / 8;
        if (f->Rate == Rate && f->Channels == Channels && f->ContainerBits == ContainerBits && f->ValidBits == ValidBits &&
            (f->IsFloat != 0) == (IsFloat != 0) && align == BlockAlign && (ULONGLONG)f->Rate * align == AvgBytesPerSec) return (LONG)i;
    }
    return -1;
}

/* Data range intersection: the first entry of the requested representation that fits the
 * client's KSDATARANGE_AUDIO limits (when it has them), or -1. */
static inline LONG PnWavePickFormat(const PN_WAVE_FORMAT *T, ULONG Count, BOOLEAN WantFloat, BOOLEAN Limited, ULONG MinRate, ULONG MaxRate,
                                    ULONG MinBits, ULONG MaxBits, ULONG MaxChannels)
{
    for (ULONG i = 0; i < Count; i++) {
        const PN_WAVE_FORMAT *f = &T[i];
        if ((f->IsFloat != 0) != (WantFloat != 0)) continue;
        if (Limited && (f->Rate < MinRate || f->Rate > MaxRate || f->ContainerBits < MinBits || f->ContainerBits > MaxBits || f->Channels > MaxChannels)) continue;
        return (LONG)i;
    }
    return -1;
}

/* Capabilities read back from the registry (written by an older or newer build, or edited
 * by hand): reject a wrong size, clamp the SEP count, drop malformed SEPs. Returns TRUE if
 * any SEP is left. */
static inline BOOLEAN PnWaveSanitizeCachedCaps(PNA2DP_CAPABILITIES *C)
{
    if (C->Size != sizeof(*C)) { C->SepCount = 0; return FALSE; }
    ULONG n = 0;
    for (ULONG i = 0; i < C->SepCount && i < PNA2DP_MAX_REMOTE_SEPS; i++) {
        if (C->Seps[i].CodecInfoLength > sizeof(C->Seps[i].CodecInfo) || C->Seps[i].Codec >= PNA2DP_CODEC_MAX) continue;
        if (n != i) C->Seps[n] = C->Seps[i];
        n++;
    }
    C->SepCount = n;
    return n != 0;
}

/* ---- endpoint notification coalescing (contract C6) ------------------------------- */

#define PN_AUDIO_NOTIFY_FORMATS 0x1
#define PN_AUDIO_NOTIFY_JACK    0x2

typedef struct _PN_AUDIO_NOTIFY {
    volatile LONG Pending;      /* PN_AUDIO_NOTIFY_* not yet handled */
    volatile LONG Queued;       /* the worker is queued or running */
} PN_AUDIO_NOTIFY;

/* Any IRQL. Records Bits; TRUE when the caller must queue the worker (it was idle). */
static inline BOOLEAN PnAudioNotifyRequest(PN_AUDIO_NOTIFY *N, LONG Bits)
{
    InterlockedOr(&N->Pending, Bits);
    return InterlockedCompareExchange(&N->Queued, 1, 0) == 0;
}

/* Worker side: the bits to handle now, or 0 when the worker must stop (it no longer owns
 * Queued). A request that arrives while the worker is clearing Queued either sees Queued
 * still set (and is picked up here) or queues a new worker; none is lost. At most two
 * passes: after re-taking Queued nobody else can clear Pending. */
static inline LONG PnAudioNotifyTake(PN_AUDIO_NOTIFY *N)
{
    for (ULONG pass = 0; pass < 2; pass++) {
        LONG bits = InterlockedExchange(&N->Pending, 0);
        if (bits) return bits;
        InterlockedExchange(&N->Queued, 0);
        if (N->Pending == 0 || InterlockedCompareExchange(&N->Queued, 1, 0) != 0) return 0;
    }
    /* not reached: after re-taking Queued the second pass finds the bits */
    LONG bits = InterlockedExchange(&N->Pending, 0);
    if (!bits) InterlockedExchange(&N->Queued, 0);
    return bits;
}
