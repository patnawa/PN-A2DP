/* Bounded reconnect scheduling shared with deterministic tests. SPDX-License-Identifier: MIT */
#pragma once
static inline unsigned PnReconnectDelayMs(unsigned attempts, unsigned configuredMs) {
    if (attempts >= 10) return 30000;
    unsigned base = configuredMs > 25000 ? 25000 : configuredMs;
    return base + 500 * (attempts + 1);
}

typedef struct PnHostRetry {
    unsigned failures;
    unsigned long long next;
    unsigned long long started;
} PnHostRetry;
static inline void PnHostRetryFailed(PnHostRetry *s, unsigned long long now) {
    if (s->failures < 5) ++s->failures;
    unsigned delay = 2000u << (s->failures-1);
    if (delay > 30000) delay = 30000;
    s->next = now + delay;
}
static inline void PnHostRetryHealthy(PnHostRetry *s, unsigned long long now) {
    if (now >= s->started && now-s->started >= 30000) s->failures = 0;
}

/* Setup retries of the A2DP stream (signalling up or not): capped exponential back-off
 * from its own failure counter, which only a stretch of continuous streaming resets. */
typedef struct PnSetupRetry {
    unsigned failures;
    int streaming;
    unsigned long long streamingSince;   /* ms */
} PnSetupRetry;
#define PN_SETUP_RETRY_BASE_MS     1500u
#define PN_SETUP_RETRY_MAX_MS      30000u
#define PN_SETUP_RETRY_HEALTHY_MS  30000u
static inline unsigned PnSetupRetryDelayMs(unsigned failures) {
    if (failures == 0) return PN_SETUP_RETRY_BASE_MS;
    if (failures >= 5) return PN_SETUP_RETRY_MAX_MS;
    unsigned delay = PN_SETUP_RETRY_BASE_MS << failures;
    return delay > PN_SETUP_RETRY_MAX_MS ? PN_SETUP_RETRY_MAX_MS : delay;
}
/* A setup attempt failed: returns the delay before the next one. */
static inline unsigned PnSetupRetryFailed(PnSetupRetry *s) {
    unsigned delay = PnSetupRetryDelayMs(s->failures);
    if (s->failures < 16) ++s->failures;
    s->streaming = 0;
    return delay;
}
/* Called with the streaming state whenever it is observed. A stretch that lasted long
 * enough counts when it ends, too. */
static inline void PnSetupRetryObserve(PnSetupRetry *s, unsigned long long nowMs, int streaming) {
    if (!streaming) {
        if (s->streaming && nowMs >= s->streamingSince && nowMs - s->streamingSince >= PN_SETUP_RETRY_HEALTHY_MS) s->failures = 0;
        s->streaming = 0;
        return;
    }
    if (!s->streaming) { s->streaming = 1; s->streamingSince = nowMs; return; }
    if (nowMs >= s->streamingSince && nowMs - s->streamingSince >= PN_SETUP_RETRY_HEALTHY_MS) s->failures = 0;
}

/* Media MTU vs baseband packets. An L2CAP SDU of MTU bytes plus the 4-byte basic L2CAP
 * header should fill whole baseband packets: 3-DH5 carries 1021 bytes, 2-DH5 679, DH5
 * 339. The cap comes from the local LMP features (bit 25 EDR 2 Mb/s, bit 26 EDR 3 Mb/s):
 *   3 Mb/s EDR     1017 = one 3-DH5
 *   2 Mb/s only     675 = one 2-DH5
 *   BR only        1013 = three DH5 (a single DH5 SDU, 335 bytes, wastes RTP/codec headers)
 *   unknown        1017 (no radio info: assume the common EDR controller)
 * PN_MEDIA_MTU_NO_CAP stays for callers without a link. */
#define PN_MEDIA_MTU_NO_CAP   0xFFFFu
#define PN_MEDIA_MTU_PREF     1024u
#define PN_LMP_EDR_2MBPS      (1ull << 25)
#define PN_LMP_EDR_3MBPS      (1ull << 26)
#define PN_MEDIA_MTU_3DH5     1017u
#define PN_MEDIA_MTU_2DH5      675u
#define PN_MEDIA_MTU_3XDH5    1013u
static inline unsigned short PnMediaMtuCap(int radioInfoValid, unsigned long long lmpFeatures) {
    if (!radioInfoValid || !lmpFeatures) return (unsigned short)PN_MEDIA_MTU_3DH5;
    if (lmpFeatures & PN_LMP_EDR_3MBPS) return (unsigned short)PN_MEDIA_MTU_3DH5;
    if (lmpFeatures & PN_LMP_EDR_2MBPS) return (unsigned short)PN_MEDIA_MTU_2DH5;
    return (unsigned short)PN_MEDIA_MTU_3XDH5;
}
/* MTU we ask for when opening or accepting the media channel. */
static inline unsigned short PnPreferredMediaMtu(int radioInfoValid, unsigned long long lmpFeatures) {
    unsigned short cap = PnMediaMtuCap(radioInfoValid, lmpFeatures);
    return cap < PN_MEDIA_MTU_PREF ? cap : (unsigned short)PN_MEDIA_MTU_PREF;
}
/* Cap of the MTU handed to the pump: one 3-DH5 (or three DH5) where the radio has it. A
 * 2 Mb/s-only radio keeps the remote's MTU, as before T12: there the SDU limit goes to
 * the SBC packing, which weighs the MTU-filling packet's slot cost against one 2-DH5
 * packet; a 675 cap would remove that choice (review round 5 #6). */
static inline unsigned short PnPumpMediaMtuCap(int radioInfoValid, unsigned long long lmpFeatures) {
    unsigned short cap = PnMediaMtuCap(radioInfoValid, lmpFeatures);
    return cap == PN_MEDIA_MTU_2DH5 ? (unsigned short)PN_MEDIA_MTU_NO_CAP : cap;
}
/* MTU handed to the pump: the remote's outgoing MTU, capped; fallback when not open. */
static inline unsigned short PnEffectiveMediaMtu(unsigned short remoteOutMtu, unsigned short cap, unsigned short fallback) {
    if (!remoteOutMtu) return fallback;
    return remoteOutMtu < cap ? remoteOutMtu : cap;
}
