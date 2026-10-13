/* Lane W core tests: WaveRT stream clock, presentation position, packet starvation,
 * timer period and topology volume rules (inc/wave_math.h); render format table,
 * cached capabilities and endpoint notification coalescing (inc/wave_formats.h).
 * Included once from core_test.cpp. SPDX-License-Identifier: MIT */
#include "wave_math.h"
#include "wave_formats.h"
#include <string.h>

/* review round 1: a check that failed before the fix */
#define RED_W7(c) do { ++checks; if (!(c)) { ++failures; printf("RED[R1-7] line %d: %s\n", __LINE__, #c); } } while (0)

static unsigned long long WTestRand(unsigned long long *s)
{
    *s = *s * 6364136223846793005ull + 1442695040888963407ull;
    return *s >> 33;
}

/* W-P1: over an hour of jittered timer ticks the frames handed out must equal the
 * frames due for the total elapsed time (no carried rounding bias). */
static void TestWaveClockDrift()
{
    static const ULONG rates[] = { 44100, 48000, 88200, 96000 };
    for (unsigned r = 0; r < sizeof(rates) / sizeof(rates[0]); r++) {
        for (unsigned mode = 0; mode < 3; mode++) {
            unsigned long long seed = 12345 + r * 7 + mode;
            ULONGLONG rem = 0, frames = 0, total = 0;
            ULONGLONG base = mode == 2 ? 20000 : 10000;
            while (total < 3600ull * PN_WAVE_HNS_PER_SEC) {
                ULONGLONG dt = base;
                if (mode) {
                    dt = base - 3000 + WTestRand(&seed) % 6001;          /* +-300 us jitter */
                    if (WTestRand(&seed) % 1000 == 0) dt += 40000;        /* occasional 4 ms DPC delay */
                }
                frames += PnWaveFramesDue(dt, rates[r], &rem);
                total += dt;
            }
            ULONGLONG expected = total * rates[r] / PN_WAVE_HNS_PER_SEC;
            long long diff = (long long)(frames - expected);
            if (diff != 0) printf("W-P1 drift: %u Hz mode %u: %lld frames after %llu s (%.1f ms)\n", rates[r], mode, diff,
                                  total / PN_WAVE_HNS_PER_SEC, diff * 1000.0 / rates[r]);
            CHECK(diff == 0);
            CHECK(rem < PN_WAVE_HNS_PER_SEC);
        }
    }
    /* a very late tick is bounded (no 64-bit overflow, no runaway) */
    ULONGLONG rem = 0;
    ULONGLONG f = PnWaveFramesDue(1000ull * PN_WAVE_HNS_PER_SEC, 384000, &rem);
    CHECK(f <= 384000ull * 10 && rem < PN_WAVE_HNS_PER_SEC);
    rem = 0;
    CHECK(PnWaveFramesDue(12345, 0, &rem) == 0 && rem == 0);
}

/* W-B1: notification lateness stays within 2 ms; short packets keep 1 ms ticks. */
static void TestWaveTickPeriod()
{
    CHECK(PnWaveTickPeriodHns(0) == 10000);          /* polled (no notifications): position granularity */
    CHECK(PnWaveTickPeriodHns(30000) == 10000);      /* 3 ms packets */
    CHECK(PnWaveTickPeriodHns(49999) == 10000);
    CHECK(PnWaveTickPeriodHns(50000) == 20000);      /* 5 ms and longer: 2 ms */
    CHECK(PnWaveTickPeriodHns(100000) == 20000);     /* the common 10 ms engine period */
    for (ULONG i = 0; i <= 2000000; i += 997) {
        ULONG p = PnWaveTickPeriodHns(i);
        CHECK(p >= 10000 && p <= 20000);
        if (i && i < 50000) CHECK(p == 10000);
    }
}

/* W-W7: in packet mode a packet the engine has not written when playback reaches it
 * is replaced by silence for its whole duration, and counted once. */
static void TestWavePacketStarvation()
{
    PN_WAVE_STARVATION s = {};
    CHECK(!PnWavePacketSilent(&s, 3, TRUE, 3));
    CHECK(PnWavePacketSilent(&s, 4, TRUE, 3));            /* missed deadline: stale previous cycle */
    CHECK(s.Starved == 1);
    CHECK(PnWavePacketSilent(&s, 4, TRUE, 5));            /* a later write of packet 5 does not revive packet 4 */
    CHECK(s.Starved == 1);
    CHECK(!PnWavePacketSilent(&s, 5, TRUE, 5));
    CHECK(PnWavePacketSilent(&s, 7, TRUE, 5) && s.Starved == 2);
    PN_WAVE_STARVATION n = {};
    CHECK(!PnWavePacketSilent(&n, 9, FALSE, 0));          /* engine not using SetWritePacket */
    CHECK(n.Starved == 0);
    PN_WAVE_STARVATION g = {};                            /* persistent starvation: protocol mismatch, give up */
    for (ULONGLONG p = 10; p < 10 + PN_WAVE_STARVE_GIVE_UP; p++) CHECK(PnWavePacketSilent(&g, p, TRUE, 9));
    CHECK(g.Disabled && g.Starved == PN_WAVE_STARVE_GIVE_UP);
    CHECK(!PnWavePacketSilent(&g, 30, TRUE, 9) && g.Starved == PN_WAVE_STARVE_GIVE_UP);
    PN_WAVE_STARVATION h = {};                            /* isolated misses never give up */
    for (ULONGLONG p = 1; p < 200; p++) PnWavePacketSilent(&h, p, TRUE, (p % 3) ? (ULONG)p : (ULONG)p - 1);
    CHECK(!h.Disabled && h.Starved == 66);
    PN_WAVE_STARVATION w1 = {}, w2 = {};                  /* packet numbers wrap at 32 bits */
    CHECK(PnWavePacketSilent(&w1, 0x100000000ull, TRUE, 0xFFFFFFFFu));
    CHECK(!PnWavePacketSilent(&w2, 0x100000000ull, TRUE, 0));

    /* simulated run: 10 ms packets, the engine writes packet pc + 1 while packet pc plays,
     * but misses the deadline of packet 6 (its SetWritePacket(6) is rejected as late) */
    const ULONG rate = 48000, block = 4, packet = 480 * block;
    ULONGLONG linear = 0, remHz = 0, stale = 0, silent = 0;
    ULONG last = 0;
    BOOLEAN written[64] = { TRUE };
    PN_WAVE_STARVATION st = {};
    for (ULONGLONG t = 0; t < 200; t++) {                 /* 1 ms ticks for 200 ms */
        ULONG pc = (ULONG)(linear / packet);
        if (pc + 1 != 6 && pc + 1 < 64) { written[pc + 1] = TRUE; last = pc + 1; }
        ULONG bytes = (ULONG)PnWaveFramesDue(10000, rate, &remHz) * block;
        while (bytes) {
            ULONGLONG p = linear / packet;
            ULONG seg = (ULONG)PN_MIN((ULONGLONG)bytes, (p + 1) * packet - linear);
            if (PnWavePacketSilent(&st, p, TRUE, last)) silent += seg;
            else if (p >= 64 || !written[p]) stale += seg;
            linear += seg; bytes -= seg;
        }
        PnWavePacketSilent(&st, linear / packet, TRUE, last);   /* decided on entry, before the notification */
    }
    if (stale) printf("W-W7 stale: %llu bytes of an unwritten packet played\n", stale);
    CHECK(stale == 0);
    CHECK(silent == packet);
    CHECK(st.Starved == 1);
}

/* W-A3: presentation position is interpolated to the reported QPC, monotonic per run,
 * and follows latency changes at a bounded slew rate. */
static void TestWavePresentation()
{
    const ULONG rate = 48000;
    const ULONGLONG freq = 10000000;                      /* QPC in hns for the simulation */
    PN_WAVE_PRESENTATION st = {};
    unsigned long long seed = 99;
    ULONGLONG t = 0, rem = 0, frames = 0, tickFrames = 0, tickQpc = 0;
    ULONGLONG prevPos = 0, prevQ = 0;
    BOOLEAN havePrev = FALSE;
    unsigned backwards = 0, jerky = 0, inaccurate = 0, samples = 0;
    long long worstJerk = 0, worstErr = 0;
    while (t < 20ull * freq) {
        ULONGLONG dt = 10000 - 3000 + WTestRand(&seed) % 6001;
        ULONGLONG next = t + dt;
        /* a few queries at random times before the next tick */
        for (int q = 0; q < 2; q++) {
            ULONGLONG qt = t + WTestRand(&seed) % dt;
            if (havePrev && qt < prevQ) continue;
            ULONG lat = 60000 + (ULONG)((qt % 200000) * 20000 / 200000);   /* per-write sawtooth 60..80 ms */
            if (qt >= 5 * freq) lat += 40000;                              /* delay report +40 ms */
            if (qt >= 12 * freq) lat -= 40000;                             /* and back */
            ULONGLONG pos = PnWavePresentationFrames(&st, tickFrames, tickQpc, TRUE, qt, freq, rate, lat);
            if (havePrev && prevPos) {                 /* before anything is presented the position stays 0 */
                if (pos < prevPos) backwards++;
                ULONGLONG dq = qt - prevQ;
                if (dq >= 5000) {
                    long long ideal = (long long)(dq * rate / freq);
                    long long d = (long long)pos - (long long)prevPos - ideal;
                    long long allow = ideal * 11 / 1000 + 3;              /* 1% slew + rounding */
                    if (d > allow || d < -allow) { jerky++; if ((d < 0 ? -d : d) > worstJerk) worstJerk = d < 0 ? -d : d; }
                }
            }
            /* converged windows (the 20 ms sawtooth settles with a ~1 s time constant after
             * the slew-limited ramp): ideal = frames consumed at qt minus the median latency */
            BOOLEAN window = (qt >= 4 * freq && qt < 5 * freq) || (qt >= 105 * freq / 10 && qt < 12 * freq) ||
                             (qt >= 185 * freq / 10);
            if (window) {
                ULONG median = 70000 + ((qt >= 5 * freq && qt < 12 * freq) ? 40000 : 0);
                long long ideal = (long long)(qt * rate / freq) - (long long)((ULONGLONG)median * rate / 1000000);
                long long e = (long long)pos - ideal;
                samples++;
                if (e > 48 || e < -48) { inaccurate++; if ((e < 0 ? -e : e) > worstErr) worstErr = e < 0 ? -e : e; }
            }
            prevPos = pos; prevQ = qt; havePrev = TRUE;
        }
        frames += PnWaveFramesDue(dt, rate, &rem);
        t = next;
        tickFrames = frames; tickQpc = t;
    }
    if (backwards || jerky || inaccurate)
        printf("W-A3 presentation: %u backwards, %u jerky (worst %lld frames), %u/%u off by > 1 ms (worst %lld frames)\n",
               backwards, jerky, worstJerk, inaccurate, samples, worstErr);
    CHECK(backwards == 0);
    CHECK(jerky == 0);
    CHECK(samples > 1000 && inaccurate == 0);

    /* a new run starts from the current latency; absurd inputs stay bounded */
    PN_WAVE_PRESENTATION s2 = {};
    CHECK(PnWavePresentationFrames(&s2, 4800, 0, TRUE, 0, freq, rate, 50000) == 2400);
    CHECK(PnWavePresentationFrames(&s2, 4800, 0, TRUE, 0, freq, rate, 0xFFFFFFFFu) == 2400);   /* no backwards step */
    PN_WAVE_PRESENTATION s3 = {};
    CHECK(PnWavePresentationFrames(&s3, 100, 0, FALSE, 50 * freq, freq, rate, 0xFFFFFFFFu) == 0);
    PN_WAVE_PRESENTATION s4 = {};
    CHECK(PnWavePresentationFrames(&s4, 1000, 0, FALSE, 5000, freq, rate, 0) == 1000);        /* paused: no extrapolation */
}

/* Review #7: while the pump is stopped the ring fills to 600 ms and the slew-limited
 * latency follows it; a restart flushes the ring. A new pump epoch (stream start or
 * stop) takes the latency of the new pipeline at once instead of at 1 %/s. */
static void TestWavePresentationEpoch()
{
    const ULONG rate = 48000;
    const ULONGLONG freq = 10000000;
    PN_WAVE_PRESENTATION st = {};
    ULONGLONG q = 0, pos = 0;
    PnWavePresentationSetEpoch(&st, 1);
    /* 60 s polled every 20 ms with the pump stopped: ring full (600 ms) + nothing else */
    for (; q < 60 * freq; q += 200000) pos = PnWavePresentationFrames(&st, q * rate / freq, q, TRUE, q, freq, rate, 600000);
    long long err = (long long)pos - (long long)((q - 200000) * rate / freq - 600000ull * rate / 1000000);
    CHECK(err >= -48 && err <= 48);
    /* START flushes the ring: 40 ms pipeline + 150 ms sink delay, a new pump epoch */
    PnWavePresentationSetEpoch(&st, 2);
    pos = PnWavePresentationFrames(&st, q * rate / freq, q, TRUE, q, freq, rate, 190000);
    err = (long long)pos - (long long)(q * rate / freq - 190000ull * rate / 1000000);
    RED_W7(err >= -48 && err <= 48);
    /* the same epoch keeps smoothing, and the position never moves backwards */
    ULONGLONG prev = pos;
    q += 200000;
    pos = PnWavePresentationFrames(&st, q * rate / freq, q, TRUE, q, freq, rate, 200000);
    CHECK(pos >= prev);
    err = (long long)pos - (long long)(q * rate / freq - 190000ull * rate / 1000000);
    CHECK(err <= 48 && err >= -48 - 10);             /* moved at most the 1 %/s slew toward 200 ms */
    /* an epoch that changes to a longer pipeline holds the position rather than stepping back */
    PnWavePresentationSetEpoch(&st, 3);
    prev = pos;
    pos = PnWavePresentationFrames(&st, q * rate / freq, q, TRUE, q, freq, rate, 600000);
    CHECK(pos == prev);
}

/* W-C3: one master channel; volume snapped to the advertised 0.5 dB steps. */
static void TestTopologyVolumeRules()
{
    CHECK(PnKsChannelIsMaster(0));
    CHECK(PnKsChannelIsMaster(0xFFFFFFFFu));
    CHECK(!PnKsChannelIsMaster(1));
    CHECK(!PnKsChannelIsMaster(2));
    const LONG mn = -96 * 65536, mx = 0, st = 0x8000;
    CHECK(PnVolumeSnapDb16(-0x4001, mn, mx, st) == -0x8000);
    CHECK(PnVolumeSnapDb16(-0x3FFF, mn, mx, st) == 0);
    CHECK(PnVolumeSnapDb16(-0x4000, mn, mx, st) == 0);
    CHECK(PnVolumeSnapDb16(-20 * 65536 - 1, mn, mx, st) == -20 * 65536);
    CHECK(PnVolumeSnapDb16(1, mn, mx, st) == 0);
    CHECK(PnVolumeSnapDb16(-100 * 65536, mn, mx, st) == mn);
    CHECK(PnVolumeSnapDb16((LONG)0x80000000, mn, mx, st) == mn);
    CHECK(PnVolumeSnapDb16(0x7FFFFFFF, mn, mx, st) == mx);
    unsigned bad = 0;
    for (LONG v = mn - 70000; v <= 70000; v += 77) {
        LONG s = PnVolumeSnapDb16(v, mn, mx, st);
        if (s < mn || s > mx || ((s - mn) % st) != 0) bad++;
        else if (v >= mn && v <= mx && (s - v > st / 2 || v - s > st / 2)) bad++;
    }
    CHECK(bad == 0);
}

/* ---- W-W1: dynamic format table (inc/wave_formats.h) ------------------------------ */

static void WAddSep(PNA2DP_CAPABILITIES *c, ULONG codec, UCHAR sink)
{
    PNA2DP_REMOTE_SEP *s = &c->Seps[c->SepCount++];
    memset(s, 0, sizeof(*s));
    UCHAR mct;
    s->Seid = (UCHAR)c->SepCount;
    s->Codec = codec;
    s->TsepIsSink = sink;
    s->CodecInfoLength = (USHORT)A2dpBuildLocalCaps(codec, s->CodecInfo, sizeof(s->CodecInfo), &mct);
    c->CodecMask |= PNA2DP_CODEC_BIT(codec);
}

static PNA2DP_CAPABILITIES WCaps(ULONG codecA, ULONG codecB)
{
    PNA2DP_CAPABILITIES c;
    memset(&c, 0, sizeof(c));
    c.Size = sizeof(c);
    if (codecA) WAddSep(&c, codecA, 1);
    if (codecB) WAddSep(&c, codecB, 1);
    return c;
}

static PN_WAVE_FORMAT_INPUT WInput(const PNA2DP_CAPABILITIES *caps, BOOLEAN active, ULONG codec, ULONG rate, BOOLEAN all, BOOLEAN flt)
{
    PN_WAVE_FORMAT_INPUT in;
    memset(&in, 0, sizeof(in));
    in.Caps = caps;
    in.EnabledCodecMask = 0x1FE;   /* every codec */
    in.ActiveValid = active; in.ActiveCodec = codec; in.ActiveRate = rate;
    in.ExposeAllRates = all; in.AllowFloat = flt;
    return in;
}

static LONG WFind(const PN_WAVE_FORMAT *t, ULONG n, ULONG rate, ULONG container, ULONG valid, BOOLEAN flt)
{
    ULONG align = 2 * container / 8;
    return PnWaveFindFormat(t, n, rate, 2, container, valid, align, rate * align, flt);
}

static bool WIs16Only(const PN_WAVE_FORMAT *t, ULONG n, ULONG r0, ULONG r1)
{
    return n == 2 && t[0].Rate == r0 && t[1].Rate == r1 && t[0].ContainerBits == 16 && t[1].ContainerBits == 16 && !t[0].IsFloat && !t[1].IsFloat;
}

/* W-W1: the table is built from the sink capabilities (live, or cached in the registry
 * before the first connection), the configuration and the negotiated rate. */
static void TestWaveFormatTable()
{
    PN_WAVE_FORMAT t[PN_WAVE_MAX_FORMATS];
    ULONG n;
    unsigned bad = 0;

    /* nothing known (first start ever): today's table, unchanged */
    PN_WAVE_FORMAT_INPUT in = WInput(NULL, FALSE, 0, 0, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));

    /* SBC-only receiver (the hardware-validated CMT-SBT40D path): 48/44.1 16-bit whether
     * connected or not, at either rate */
    PNA2DP_CAPABILITIES sbc = WCaps(PNA2DP_CODEC_SBC, 0);
    ULONG sbcRates = 0;
    CHECK(PnWaveSinkSepRates(&sbc.Seps[0], &sbcRates) && (sbcRates & PNA2DP_RATE_44100) && (sbcRates & PNA2DP_RATE_48000));
    in = WInput(&sbc, FALSE, 0, 0, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));
    in = WInput(&sbc, TRUE, PNA2DP_CODEC_SBC, 44100, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));   /* a connect at 44.1 kHz does not reorder the table */
    in = WInput(&sbc, TRUE, PNA2DP_CODEC_SBC, 48000, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));
    in = WInput(&sbc, FALSE, 0, 0, TRUE, TRUE);   /* EXPOSE_ALL_RATES: every SBC rate, 16-bit */
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(n == 4 && t[0].Rate == 48000 && t[1].Rate == 44100 && t[2].Rate == 32000 && t[3].Rate == 16000 && t[3].ContainerBits == 16);

    /* LDAC receiver known only from the registry cache: hi-res formats before the first
     * connection (24-in-32 is the default), float when the pin has a float range */
    PNA2DP_CAPABILITIES ldac = WCaps(PNA2DP_CODEC_SBC, PNA2DP_CODEC_LDAC);
    ULONG ldacRates = 0;
    CHECK(PnWaveSinkSepRates(&ldac.Seps[1], &ldacRates) && (ldacRates & PNA2DP_RATE_96000) && (ldacRates & PNA2DP_RATE_88200));
    PNA2DP_CAPABILITIES cached = ldac;
    BOOLEAN haveCache = PnWaveSanitizeCachedCaps(&cached);
    CHECK(haveCache);
    in = WInput(haveCache ? &cached : NULL, FALSE, 0, 0, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    if (n < 1 || t[0].ContainerBits != 32) printf("W-W1 formats: cached LDAC sink offers %u formats, first %u-bit\n", n, n ? t[0].ContainerBits : 0);
    CHECK(n == 6 && t[0].Rate == 48000 && t[0].ContainerBits == 32 && t[0].ValidBits == 24 && !t[0].IsFloat);
    CHECK(WFind(t, n, 48000, 32, 32, TRUE) >= 0 && WFind(t, n, 44100, 32, 32, TRUE) >= 0);
    CHECK(WFind(t, n, 48000, 16, 16, FALSE) >= 0 && WFind(t, n, 44100, 16, 16, FALSE) >= 0);
    CHECK(WFind(t, n, 44100, 32, 24, FALSE) >= 0);
    CHECK(WFind(t, n, 96000, 32, 24, FALSE) < 0);   /* 96 kHz only with EXPOSE_ALL_RATES */
    in.ExposeAllRates = TRUE;
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    if (WFind(t, n, 96000, 32, 24, FALSE) < 0) printf("W-W1 formats: EXPOSE_ALL_RATES with a cached LDAC sink offers no 96 kHz hi-res\n");
    CHECK(n == PN_WAVE_MAX_FORMATS);   /* 6 rates x 3 representations fit */
    CHECK(WFind(t, n, 96000, 32, 24, FALSE) >= 0 && WFind(t, n, 88200, 32, 32, TRUE) >= 0 && WFind(t, n, 16000, 16, 16, FALSE) >= 0);
    in.ExposeAllRates = FALSE; in.AllowFloat = FALSE;   /* pin created without a float range */
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(n == 4 && WFind(t, n, 48000, 32, 32, TRUE) < 0 && WFind(t, n, 48000, 32, 24, FALSE) == 0);

    /* the sink chose LDAC at 96 kHz: the negotiated rate is offered, first */
    in = WInput(&ldac, TRUE, PNA2DP_CODEC_LDAC, 96000, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(n == 9 && t[0].Rate == 96000 && t[0].ValidBits == 24 && t[3].Rate == 48000 && t[6].Rate == 44100);
    /* the stream fell back to SBC on that sink: the table stays the same (no format churn) */
    PN_WAVE_FORMAT t2[PN_WAVE_MAX_FORMATS];
    in = WInput(&ldac, FALSE, 0, 0, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    in = WInput(&ldac, TRUE, PNA2DP_CODEC_SBC, 48000, FALSE, TRUE);
    ULONG n2 = PnWaveBuildFormats(&in, t2, PN_WAVE_MAX_FORMATS);
    CHECK(n == n2 && memcmp(t, t2, n * sizeof(t[0])) == 0);
    /* LDAC disabled in the configuration, or only offered by a source SEP: 16-bit only */
    in = WInput(&ldac, FALSE, 0, 0, FALSE, TRUE);
    in.EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));
    PNA2DP_CAPABILITIES src = WCaps(PNA2DP_CODEC_SBC, 0);
    WAddSep(&src, PNA2DP_CODEC_LDAC, 0);
    in = WInput(&src, FALSE, 0, 0, TRUE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(n == 4 && WFind(t, n, 96000, 16, 16, FALSE) < 0 && t[0].ContainerBits == 16);
    /* aptX (24-bit encoder input) is hi-res; AAC (16-bit) is not */
    PNA2DP_CAPABILITIES aptx = WCaps(PNA2DP_CODEC_SBC, PNA2DP_CODEC_APTX), aac = WCaps(PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC);
    in = WInput(&aptx, FALSE, 0, 0, FALSE, TRUE);
    CHECK(PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS) == 6 && t[0].ValidBits == 24);
    in = WInput(&aac, TRUE, PNA2DP_CODEC_AAC, 48000, FALSE, TRUE);
    n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
    CHECK(WIs16Only(t, n, 48000, 44100));

    /* sweep: any input gives a well-formed table that always has 16-bit 48 and 44.1 kHz,
     * fits, has no duplicates, and each entry is found by its own fields */
    static const ULONG rates[] = { 0, 16000, 32000, 44100, 48000, 88200, 96000, 192000 };
    static const ULONG codecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_LDAC };
    for (unsigned a = 0; a < 5; a++) for (unsigned b = 0; b < 5; b++) for (unsigned r = 0; r < 8; r++) for (unsigned f = 0; f < 8; f++) {
        PNA2DP_CAPABILITIES c = WCaps(codecs[a], codecs[b]);
        in = WInput((f & 4) ? &c : NULL, r != 0, codecs[b], rates[r], (f & 1) != 0, (f & 2) != 0);
        n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);
        if (n > PN_WAVE_MAX_FORMATS || WFind(t, n, 48000, 16, 16, FALSE) < 0 || WFind(t, n, 44100, 16, 16, FALSE) < 0) bad++;
        for (ULONG i = 0; i < n && i < PN_WAVE_MAX_FORMATS; i++) {
            if (WFind(t, n, t[i].Rate, t[i].ContainerBits, t[i].ValidBits, t[i].IsFloat) != (LONG)i) bad++;
            if (t[i].Channels != 2 || (t[i].IsFloat && (t[i].ContainerBits != 32 || t[i].ValidBits != 32 || !in.AllowFloat))) bad++;
            if (!A2dpHzToRateMask(t[i].Rate)) bad++;
        }
    }
    CHECK(bad == 0);
}

/* W-W1: float is accepted when the table has it, every field must agree, and the KS
 * subformat must agree with the WAVEFORMATEX tag. */
static void TestWaveFormatMatch()
{
    PNA2DP_CAPABILITIES ldac = WCaps(PNA2DP_CODEC_SBC, PNA2DP_CODEC_LDAC);
    PN_WAVE_FORMAT t[PN_WAVE_MAX_FORMATS];
    PN_WAVE_FORMAT_INPUT in = WInput(&ldac, FALSE, 0, 0, TRUE, TRUE);
    ULONG n = PnWaveBuildFormats(&in, t, PN_WAVE_MAX_FORMATS);

    CHECK(PnWaveFormatKind(PN_WAVE_SUB_PCM, PN_WAVE_TAG_PCM, PN_WAVE_SUB_OTHER) == 0);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_PCM, PN_WAVE_TAG_EXTENSIBLE, PN_WAVE_SUB_PCM) == 0);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_WAVEFORMATEX, PN_WAVE_TAG_PCM, PN_WAVE_SUB_OTHER) == 0);
    int fk1 = PnWaveFormatKind(PN_WAVE_SUB_FLOAT, PN_WAVE_TAG_EXTENSIBLE, PN_WAVE_SUB_FLOAT);
    int fk2 = PnWaveFormatKind(PN_WAVE_SUB_FLOAT, PN_WAVE_TAG_IEEE_FLOAT, PN_WAVE_SUB_OTHER);
    int fk3 = PnWaveFormatKind(PN_WAVE_SUB_WAVEFORMATEX, PN_WAVE_TAG_IEEE_FLOAT, PN_WAVE_SUB_OTHER);
    if (fk1 != 1 || fk2 != 1 || fk3 != 1) printf("W-W1 match: IEEE float formats rejected (%d %d %d)\n", fk1, fk2, fk3);
    CHECK(fk1 == 1 && fk2 == 1 && fk3 == 1);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_PCM, PN_WAVE_TAG_EXTENSIBLE, PN_WAVE_SUB_FLOAT) == -1);   /* contradictory */
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_FLOAT, PN_WAVE_TAG_PCM, PN_WAVE_SUB_OTHER) == -1);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_PCM, 0x0002 /* ADPCM */, PN_WAVE_SUB_OTHER) == -1);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_OTHER, PN_WAVE_TAG_PCM, PN_WAVE_SUB_OTHER) == -1);
    CHECK(PnWaveFormatKind(PN_WAVE_SUB_PCM, PN_WAVE_TAG_EXTENSIBLE, PN_WAVE_SUB_OTHER) == -1);

    LONG fl = WFind(t, n, 48000, 32, 32, TRUE);
    CHECK(fl >= 0 && t[fl].IsFloat);
    CHECK(WFind(t, n, 48000, 32, 32, FALSE) < 0);          /* 32-bit integer PCM is not offered */
    CHECK(WFind(t, n, 48000, 32, 24, TRUE) < 0);
    CHECK(PnWaveFindFormat(t, n, 48000, 2, 16, 16, 4, 48000 * 4 + 1, FALSE) < 0);   /* inconsistent byte rate */
    CHECK(PnWaveFindFormat(t, n, 48000, 2, 16, 16, 8, 48000 * 8, FALSE) < 0);       /* inconsistent block align */
    CHECK(PnWaveFindFormat(t, n, 48000, 1, 16, 16, 2, 48000 * 2, FALSE) < 0);       /* mono is not offered */
    PN_WAVE_FORMAT_INPUT none = WInput(NULL, FALSE, 0, 0, FALSE, FALSE);
    PN_WAVE_FORMAT t16[PN_WAVE_MAX_FORMATS];
    ULONG n16 = PnWaveBuildFormats(&none, t16, PN_WAVE_MAX_FORMATS);
    CHECK(WFind(t16, n16, 48000, 32, 32, TRUE) < 0 && WFind(t16, n16, 44100, 16, 16, FALSE) == 1);

    CHECK(fl >= 0 && PnWavePickFormat(t, n, TRUE, FALSE, 0, 0, 0, 0, 0) == fl);
    LONG p16 = PnWavePickFormat(t, n, FALSE, TRUE, 8000, 192000, 8, 16, 2);
    CHECK(p16 >= 0 && t[p16].ContainerBits == 16 && t[p16].Rate == 48000);
    LONG p96 = PnWavePickFormat(t, n, FALSE, TRUE, 96000, 96000, 8, 32, 2);
    CHECK(p96 >= 0 && t[p96].Rate == 96000 && t[p96].ValidBits == 24);
    CHECK(PnWavePickFormat(t16, n16, TRUE, FALSE, 0, 0, 0, 0, 0) == -1);
    CHECK(PnWavePickFormat(t, n, FALSE, TRUE, 8000, 192000, 8, 32, 1) == -1);       /* client wants mono */
}

/* W-W1: capabilities cached in the registry are checked before use. */
static void TestWaveCachedCaps()
{
    PNA2DP_CAPABILITIES c = WCaps(PNA2DP_CODEC_SBC, PNA2DP_CODEC_LDAC);
    CHECK(PnWaveSanitizeCachedCaps(&c) && c.SepCount == 2);
    PNA2DP_CAPABILITIES s = c; s.Size = sizeof(s) - 4;
    CHECK(!PnWaveSanitizeCachedCaps(&s) && s.SepCount == 0);
    PNA2DP_CAPABILITIES big = c; big.SepCount = 40;
    CHECK(PnWaveSanitizeCachedCaps(&big) && big.SepCount <= PNA2DP_MAX_REMOTE_SEPS);
    PNA2DP_CAPABILITIES m = c; m.Seps[0].CodecInfoLength = 200;
    CHECK(PnWaveSanitizeCachedCaps(&m) && m.SepCount == 1 && m.Seps[0].Codec == PNA2DP_CODEC_LDAC);
    PNA2DP_CAPABILITIES e; memset(&e, 0, sizeof(e)); e.Size = sizeof(e);
    CHECK(!PnWaveSanitizeCachedCaps(&e));
}

/* C6: endpoint notifications coalesce into one PASSIVE worker; none is lost. */
static void TestAudioNotify()
{
    PN_AUDIO_NOTIFY q = {};
    CHECK(PnAudioNotifyRequest(&q, PN_AUDIO_NOTIFY_FORMATS));      /* idle: queue the worker */
    CHECK(!PnAudioNotifyRequest(&q, PN_AUDIO_NOTIFY_JACK));        /* coalesced */
    CHECK(PnAudioNotifyTake(&q) == (PN_AUDIO_NOTIFY_FORMATS | PN_AUDIO_NOTIFY_JACK));
    CHECK(!PnAudioNotifyRequest(&q, PN_AUDIO_NOTIFY_JACK));        /* worker still running: it picks this up */
    CHECK(PnAudioNotifyTake(&q) == PN_AUDIO_NOTIFY_JACK);
    CHECK(PnAudioNotifyTake(&q) == 0 && q.Queued == 0);             /* done: idle again */
    CHECK(PnAudioNotifyRequest(&q, PN_AUDIO_NOTIFY_FORMATS));
    CHECK(PnAudioNotifyTake(&q) == PN_AUDIO_NOTIFY_FORMATS && PnAudioNotifyTake(&q) == 0 && q.Queued == 0);
}

/* ---- Sound-quality review: the WaveRT side ---------------------------------------- */
/* a check that failed before the fix for finding n */
#define RED_W(id, c) do { ++checks; if (!(c)) { ++failures; printf("RED[%s] line %d: %s\n", id, __LINE__, #c); } } while (0)

/* The Tick loop of audio_wave.cpp over the production wave_math.h: 10 ms packets, a 2-packet
 * buffer, 2 ms ticks, an engine that writes packet GetPacketCount() + 1 EngineUs after each
 * notification. One tick runs LateMs late. Returns the frames played as starvation silence;
 * *Consumed: frames consumed by the end (the clock must catch up). */
static ULONG WaveLateTickSim(ULONG LateMs, ULONG EngineUs, BOOLEAN Hold, ULONGLONG *Consumed)
{
    const ULONG rate = 48000, bpf = 4, packet = 480 * bpf, bufferSize = 2 * packet;
    ULONGLONG linear = 0, rem = 0, pc = 0;
    ULONG lastWritten = 1, owed = 0, silentBytes = 0;   /* the engine pre-wrote packets 0 and 1 */
    LONGLONG notifyAt = -1;                              /* hns of the pending engine wake */
    PN_WAVE_STARVATION st = {};
    PN_WAVE_LATE_HOLD hold = {};
    LONGLONG now = 0, lastTick = 0;
    for (int tick = 1; tick <= 200; ++tick) {
        LONGLONG due = (LONGLONG)tick * 20000 + (tick >= 100 ? (LONGLONG)LateMs * 10000 : 0);
        if (tick == 100) due = 100 * 20000 + (LONGLONG)LateMs * 10000;
        /* the engine writes when it wakes (between ticks) */
        if (notifyAt >= 0 && notifyAt <= due) {
            ULONG want = (ULONG)pc + 1;
            if (want > lastWritten) lastWritten = want;   /* SetWritePacket(pc + 1) is accepted */
            notifyAt = -1;
        }
        now = due;
        ULONGLONG bytes = PnWaveFramesDue((ULONGLONG)(now - lastTick), rate, &rem) * bpf + owed;
        owed = 0;
        lastTick = now;
        if (bytes > bufferSize) bytes = bufferSize;
        ULONG remaining = (ULONG)bytes;
        BOOLEAN held = FALSE;
        while (remaining) {
            ULONGLONG p = linear / packet;
            if (Hold && linear % packet == 0 && PnWaveLateHold(&hold, p, pc, lastWritten, (ULONGLONG)now, remaining, packet)) { held = TRUE; break; }
            ULONG seg = PN_MIN(remaining, packet - (ULONG)(linear % packet));
            if (PnWavePacketSilent(&st, p, TRUE, lastWritten)) silentBytes += seg;
            linear += seg;
            remaining -= seg;
        }
        /* a tick that ends exactly where such a packet starts holds there as well */
        if (Hold && !held && linear % packet == 0 && PnWaveLateHold(&hold, linear / packet, pc, lastWritten, (ULONGLONG)now, 0, packet)) held = TRUE;
        if (held) owed = remaining;
        if (!held) PnWavePacketSilent(&st, linear / packet, TRUE, lastWritten);
        ULONGLONG newPc = linear / packet;
        if (held && newPc) newPc--;
        if (newPc != pc) { pc = newPc; notifyAt = now + EngineUs * 10; }
    }
    *Consumed = linear / bpf + owed / bpf;
    return silentBytes / bpf;
}

static void TestWaveQualityReview()
{
    /* #21: the starvation check re-arms once the engine writes on time again */
    {
        PN_WAVE_STARVATION g = {};
        for (ULONGLONG p = 10; p < 10 + PN_WAVE_STARVE_GIVE_UP; p++) CHECK(PnWavePacketSilent(&g, p, TRUE, 9));
        CHECK(g.Disabled);
        /* the packet that gave up stays silent for its whole duration */
        CHECK(PnWavePacketSilent(&g, 10 + PN_WAVE_STARVE_GIVE_UP - 1, TRUE, 9));
        ULONGLONG p = 30;
        for (ULONG i = 0; i < PN_WAVE_STARVE_REARM; ++i, ++p) CHECK(!PnWavePacketSilent(&g, p, TRUE, (ULONG)p));
        RED_W("Q21", !g.Disabled);
        CHECK(PnWavePacketSilent(&g, p + 1, TRUE, (ULONG)p) && g.Starved == PN_WAVE_STARVE_GIVE_UP + 1);
        /* an engine that stopped reporting writes never re-arms it (protocol mismatch) */
        PN_WAVE_STARVATION m = {};
        for (ULONGLONG q = 10; q < 300; q++) PnWavePacketSilent(&m, q, TRUE, 9);
        CHECK(m.Disabled && m.Starved == PN_WAVE_STARVE_GIVE_UP);
    }
    /* #20: a late tick never silences a packet the engine was not asked to write */
    {
        ULONGLONG consumed = 0;
        for (ULONG late : { 9u, 12u, 14u, 16u, 18u, 20u }) {
            ULONG holdSilent = WaveLateTickSim(late, 300, TRUE, &consumed);
            ULONGLONG expect = (ULONGLONG)(200 * 20000 + late * 10000) * 48000 / 10000000;
            /* nothing silenced; the clock catches up (a tick later than the 20 ms buffer is
             * clamped to one buffer, as before) */
            CHECK(holdSilent == 0 && (late + 2 > 20 || consumed == expect));
        }
        ULONG oldSilent = WaveLateTickSim(14, 300, FALSE, &consumed);
        CHECK(oldSilent == 480);   /* what the late tick did before: 10 ms of silence */
        RED_W("Q20", WaveLateTickSim(14, 300, TRUE, &consumed) == 0);
        /* an engine that really misses its deadline still starves, after the hold */
        CHECK(WaveLateTickSim(14, 30000, TRUE, &consumed) >= 480);
    }
    /* #23/#24: the start hold */
    CHECK(PnWaveStartHold(10, 20, FALSE) && !PnWaveStartHold(10, 20, TRUE) && !PnWaveStartHold(20, 20, FALSE) && !PnWaveStartHold(10, 0, FALSE));
    static_assert(PN_WAVE_START_HOLD_MS >= 450 && PN_WAVE_START_HOLD_MS <= 600, "covers the Sony's START with a sniff exit (441 ms)");
    /* #4: fade lengths */
    CHECK(PnWaveFadeFrames(48000, 480, 1024) == 96 && PnWaveFadeFrames(192000, 1920, 512) == 384);
    CHECK(PnWaveFadeFrames(48000, 64, 1024) == 64 && PnWaveFadeFrames(48000, 0, 50) == 50);
}

static void TestLaneW()
{
    TestWaveClockDrift(); TestWaveTickPeriod(); TestWavePacketStarvation(); TestWavePresentation(); TestTopologyVolumeRules();
    TestWaveFormatTable(); TestWaveFormatMatch(); TestWaveCachedCaps(); TestAudioNotify();
    TestWavePresentationEpoch();
    TestWaveQualityReview();
}
