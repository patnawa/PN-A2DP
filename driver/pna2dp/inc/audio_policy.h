/* Pure endpoint/transport decisions shared with regression tests.
 * SPDX-License-Identifier: MIT */
#pragma once

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
} PN_QUALITY_WINDOW;

/* Return +1 to reduce bitrate, -1 to recover, 0 to keep it. Only continuously
 * observed healthy traffic earns recovery time; idle and long scheduling gaps
 * cannot masquerade as a stable link. The caller owns this state on one thread. */
static inline int PnQualityObserve(PN_QUALITY_WINDOW *S, ULONG ElapsedMs,
    BOOLEAN Traffic, BOOLEAN Congested, BOOLEAN Enable, ULONG Level, ULONG MaxLevel)
{
    if (ElapsedMs > 1000) { S->GoodMs = S->BadMs = 0; ElapsedMs = 250; }
    S->SinceChangeMs = S->SinceChangeMs >= 30000 ? 30000 : S->SinceChangeMs + ElapsedMs;
    if (Congested) {
        S->GoodMs = 0;
        S->BadMs = S->BadMs >= 1000 ? 1000 : S->BadMs + ElapsedMs;
    } else {
        S->BadMs = 0;
        S->GoodMs = Traffic ? (S->GoodMs >= 30000 ? 30000 : S->GoodMs + ElapsedMs) : 0;
    }
    if (Enable && S->BadMs >= 1000 && S->SinceChangeMs >= 2000 && Level < MaxLevel) {
        S->BadMs = S->SinceChangeMs = 0;
        return 1;
    }
    if (Enable && Level && S->GoodMs >= 30000 && S->SinceChangeMs >= 30000) {
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
