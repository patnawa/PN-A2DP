/*
 * config_policy.h - configuration defaults, validation and latency profiles.
 *
 * One source of truth for the driver (regcfg.cpp RegCfgDefaults / RegCfgValidate), the tools
 * (tools/common/pnctl.cpp ConfigDefaults, presets, text round trip, panel) and the tests.
 * Pure: only PNA2DP_CONFIG from pna2dp_public.h; builds in kernel and user mode.
 *
 * Defaults (0.1.0.22): every codec starts at its best level and ABR is the safety net. LDAC
 * HQ (990/909 kbps), AAC at the sink's maximum up to 320 kbps CBR (A2dpSelectConfig takes the
 * smaller of the two; the pump caps it to the MTU), SBC at the receiver's maximum bitpool
 * (MaxBitpool 0; the MTU and baseband plan still cap it), aptX HD before aptX. The SBC channel
 * mode default is unchanged (joint / stereo): dual channel would reach the dual-channel ABR
 * floor (known issue #31). Defaults apply to devices without a saved configuration only.
 *
 * Latency profiles are a TargetLatencyMs / MaxLatencyMs pair; PNA2DP_CONFIG (ABI 1) is
 * unchanged and the driver never stores a profile id. Every rule that depends on the buffer
 * derives from the effective target (audio_policy.h PnAbrSoftGateUs, PnSbcPackingTargetMs),
 * so custom values scale too.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define PN_LATENCY_PROFILE_CUSTOM    0u
#define PN_LATENCY_PROFILE_LOW       1u   /* video calls, games */
#define PN_LATENCY_PROFILE_BALANCED  2u   /* default; the 0.1.0.21 behaviour */
#define PN_LATENCY_PROFILE_MUSIC     3u   /* more buffering: rides out radio hiccups */

#define PN_LATENCY_LOW_TARGET_MS        30u
#define PN_LATENCY_LOW_MAX_MS          120u
#define PN_LATENCY_BALANCED_TARGET_MS   60u
#define PN_LATENCY_BALANCED_MAX_MS     250u
#define PN_LATENCY_MUSIC_TARGET_MS     150u
#define PN_LATENCY_MUSIC_MAX_MS        450u

/* RegCfgValidate bounds. The maximum stays PN_RING_HEADROOM_MS (100) below the pump's 600 ms
 * ring (PN_PUMP_RING_MS; regcfg.cpp asserts the relation), the target 20 ms below the maximum. */
#define PN_CONFIG_MIN_TARGET_MS          20u
#define PN_CONFIG_MAX_TARGET_MS         400u
#define PN_CONFIG_MAX_LATENCY_MS        500u
/* Speaker delay (SinkDelayMs, 0.1.0.25): 0 = automatic (the receiver's plausible reports, else
 * the per-codec default; transport_policy.h PnSinkDelayDecide), else up to this many ms. */
#define PN_CONFIG_MAX_SINK_DELAY_MS    1000u

/* RegCfgLoadDevice results meaning "no configuration stored" (STATUS_OBJECT_NAME_NOT_FOUND,
 * STATUS_OBJECT_PATH_NOT_FOUND; regcfg.cpp asserts the values). */
#define PN_CONFIG_STATUS_NAME_NOT_FOUND  ((LONG)0xC0000034L)
#define PN_CONFIG_STATUS_PATH_NOT_FOUND  ((LONG)0xC000003AL)

/* TRUE when the profile has fixed values (*TargetMs, *MaxMs); FALSE for CUSTOM or unknown. */
static inline BOOLEAN PnLatencyProfileValues(ULONG Profile, ULONG *TargetMs, ULONG *MaxMs)
{
    switch (Profile) {
    case PN_LATENCY_PROFILE_LOW:      *TargetMs = PN_LATENCY_LOW_TARGET_MS;      *MaxMs = PN_LATENCY_LOW_MAX_MS;      return TRUE;
    case PN_LATENCY_PROFILE_BALANCED: *TargetMs = PN_LATENCY_BALANCED_TARGET_MS; *MaxMs = PN_LATENCY_BALANCED_MAX_MS; return TRUE;
    case PN_LATENCY_PROFILE_MUSIC:    *TargetMs = PN_LATENCY_MUSIC_TARGET_MS;    *MaxMs = PN_LATENCY_MUSIC_MAX_MS;    return TRUE;
    default: return FALSE;
    }
}

/* The profile whose values are exactly TargetMs / MaxMs, else CUSTOM. */
static inline ULONG PnLatencyProfileOf(ULONG TargetMs, ULONG MaxMs)
{
    for (ULONG p = PN_LATENCY_PROFILE_LOW; p <= PN_LATENCY_PROFILE_MUSIC; p++) {
        ULONG t, m;
        if (PnLatencyProfileValues(p, &t, &m) && t == TargetMs && m == MaxMs) return p;
    }
    return PN_LATENCY_PROFILE_CUSTOM;
}

/* Sets both values of Profile; CUSTOM (or unknown) leaves Cfg alone. */
static inline VOID PnApplyLatencyProfile(PNA2DP_CONFIG *Cfg, ULONG Profile)
{
    ULONG t, m;
    if (PnLatencyProfileValues(Profile, &t, &m)) { Cfg->TargetLatencyMs = t; Cfg->MaxLatencyMs = m; }
}

static inline VOID PnConfigDefaults(PNA2DP_CONFIG *Cfg)
{
    RtlZeroMemory(Cfg, sizeof(*Cfg));
    Cfg->Size = sizeof(*Cfg);
    Cfg->Version = PNA2DP_ABI_VERSION;
    /* Quality first: LDAC > aptX HD > AAC > aptX > aptX LL > SBC. A codec that no host offers is skipped. */
    Cfg->CodecPriority[0] = PNA2DP_CODEC_LDAC;
    Cfg->CodecPriority[1] = PNA2DP_CODEC_APTX_HD;
    Cfg->CodecPriority[2] = PNA2DP_CODEC_AAC;
    Cfg->CodecPriority[3] = PNA2DP_CODEC_APTX;
    Cfg->CodecPriority[4] = PNA2DP_CODEC_APTX_LL;
    Cfg->CodecPriority[5] = PNA2DP_CODEC_SBC;
    Cfg->EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) |
                            PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    Cfg->Flags = PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_APPLY_LIVE | PNA2DP_CFG_SUSPEND_ON_SCO | PNA2DP_CFG_DELAY_REPORTING |
                 PNA2DP_CFG_SOFTWARE_VOLUME | PNA2DP_CFG_AUTO_QUALITY;
    Cfg->TargetLatencyMs = PN_LATENCY_BALANCED_TARGET_MS;   /* Balanced profile */
    Cfg->MaxLatencyMs = PN_LATENCY_BALANCED_MAX_MS;
    Cfg->AbrFloorBps = 128000;
    Cfg->VolumeDb16 = 0;
    Cfg->ReconnectDelayMs = 2000;
    Cfg->Sbc.MinBitpool = 2;
    Cfg->Sbc.MaxBitpool = 0;                 /* the receiver's maximum (the MTU still caps it) */
    Cfg->Aac.BitrateBps = 320000;            /* the sink's maximum up to 320 kbps */
    Cfg->Aac.Vbr = 0;                        /* CBR at the ceiling; ABR rungs below it */
    Cfg->Ldac.Eqmid = PNA2DP_LDAC_EQMID_HQ;  /* 990 / 909 kbps; ABR steps to SQ / MQ */
    Cfg->Ldac.AbrEnable = 1;
    Cfg->Ldac.FormatMask = 0;                /* automatic: the engine's precision */
}

/* RegCfgValidate: FALSE for another structure size or ABI; otherwise clamps to what the
 * driver applies. A configuration it leaves unchanged is stored exactly as sent. */
static inline BOOLEAN PnConfigValidate(PNA2DP_CONFIG *Cfg)
{
    if (Cfg->Size != sizeof(*Cfg) || Cfg->Version != PNA2DP_ABI_VERSION) return FALSE;
    if (Cfg->TargetLatencyMs < PN_CONFIG_MIN_TARGET_MS) Cfg->TargetLatencyMs = PN_CONFIG_MIN_TARGET_MS;
    if (Cfg->TargetLatencyMs > PN_CONFIG_MAX_TARGET_MS) Cfg->TargetLatencyMs = PN_CONFIG_MAX_TARGET_MS;
    if (Cfg->MaxLatencyMs < Cfg->TargetLatencyMs + 20) Cfg->MaxLatencyMs = Cfg->TargetLatencyMs + 20;
    /* the pump never lets the ring grow beyond this (PnEffectiveLatencyMs): store what applies */
    if (Cfg->MaxLatencyMs > PN_CONFIG_MAX_LATENCY_MS) Cfg->MaxLatencyMs = PN_CONFIG_MAX_LATENCY_MS;
    if (Cfg->Sbc.MinBitpool && Cfg->Sbc.MinBitpool < 2) Cfg->Sbc.MinBitpool = 2;
    if (Cfg->Sbc.MaxBitpool > 250) Cfg->Sbc.MaxBitpool = 250;
    if (Cfg->Sbc.MaxBitpool && Cfg->Sbc.MinBitpool > Cfg->Sbc.MaxBitpool) Cfg->Sbc.MinBitpool = Cfg->Sbc.MaxBitpool;
    if (Cfg->Ldac.Eqmid > PNA2DP_LDAC_EQMID_MQ && Cfg->Ldac.Eqmid != PNA2DP_LDAC_EQMID_ABR) Cfg->Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ;
    if (Cfg->VolumeDb16 > 0) Cfg->VolumeDb16 = 0;
    if (Cfg->VolumeDb16 < -96 * 0x10000) Cfg->VolumeDb16 = -96 * 0x10000;
    if (Cfg->SinkDelayCodec >= PNA2DP_CODEC_MAX) Cfg->SinkDelayMs = Cfg->SinkDelayCodec = 0;   /* automatic */
    if (Cfg->SinkDelayMs > PN_CONFIG_MAX_SINK_DELAY_MS) Cfg->SinkDelayMs = PN_CONFIG_MAX_SINK_DELAY_MS;
    BOOLEAN any = FALSE;
    for (ULONG i = 0; i < sizeof(Cfg->CodecPriority) / sizeof(Cfg->CodecPriority[0]); i++) {
        if (Cfg->CodecPriority[i] >= PNA2DP_CODEC_MAX) Cfg->CodecPriority[i] = 0;
        if (Cfg->CodecPriority[i]) any = TRUE;
    }
    if (!any) Cfg->CodecPriority[0] = PNA2DP_CODEC_SBC;
    Cfg->EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);   /* SBC is mandatory */
    Cfg->Flags |= PNA2DP_CFG_SOFTWARE_VOLUME;   /* no AVRCP absolute-volume implementation */
    if (Cfg->Flags & PNA2DP_CFG_AUTO_QUALITY) {
        Cfg->Flags |= PNA2DP_CFG_ABR_ENABLE;
        Cfg->Ldac.AbrEnable = 1;
    }
    return TRUE;
}

/* DeviceStart: the defaults are saved only when no configuration exists. Any other load
 * failure (another ABI's size, a value validation rejects, an I/O error) uses the defaults
 * in memory and keeps whatever is stored; the user's first SET_CONFIG saves as before. */
static inline BOOLEAN PnConfigShouldSaveDefaults(LONG LoadStatus)
{
    return LoadStatus == PN_CONFIG_STATUS_NAME_NOT_FOUND || LoadStatus == PN_CONFIG_STATUS_PATH_NOT_FOUND;
}
