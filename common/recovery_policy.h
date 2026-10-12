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
