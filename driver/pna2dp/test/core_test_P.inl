/* Lane P regression tests: codec host failure policy, request accounting, result
 * bounds, the AAC MTU cap, latency/ABR, SBC packing/XQ, dither and the decimator.
 * Included by core_test.cpp. SPDX-License-Identifier: MIT */

/* review round 1: checks that failed before the fix (reported, counted as failures) */
#define RED_P6(c) do { ++checks; if (!(c)) { ++failures; printf("RED[R1-6] line %d: %s\n", __LINE__, #c); } } while (0)
#define RED_P8(c) do { ++checks; if (!(c)) { ++failures; printf("RED[R1-8] line %d: %s\n", __LINE__, #c); } } while (0)
/* review round 2: checks that failed before its fixes (also used by core_test_T/G.inl) */
#define RED_R2(id, c) do { ++checks; if (!(c)) { ++failures; printf("RED[%s] line %d: %s\n", id, __LINE__, #c); } } while (0)

/* P2: one bad block is not a host failure; a run of failures quarantines the host for
 * 30 s, then 2 min, then 10 min; quarantine always expires; sustained success forgives. */
static void TestHostHealthPolicy()
{
    PN_HOST_HEALTH h = {};
    ULONGLONG now = 1000;
    /* isolated failures between good results never quarantine */
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(!PnHostHealthFailure(&h, now));
        now += 5;
        PnHostHealthSuccess(&h, now);
        now += 5;
    }
    CHECK(!PnHostHealthQuarantined(&h, now));
    for (unsigned i = 0; i + 1 < PN_HOST_FAILURE_LIMIT; ++i) { PnHostHealthFailure(&h, now); PnHostHealthSuccess(&h, now); }
    CHECK(!PnHostHealthQuarantined(&h, now));

    /* a consecutive run quarantines exactly once */
    unsigned started = 0;
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) started += PnHostHealthFailure(&h, now) ? 1 : 0;
    CHECK(started == 1 && PnHostHealthQuarantined(&h, now));
    /* late failures of the abandoned stream do not extend or restart it */
    for (unsigned i = 0; i < 20; ++i) CHECK(!PnHostHealthFailure(&h, now + 1));
    CHECK(!PnHostHealthExpire(&h, now + 29999));
    CHECK(PnHostHealthQuarantined(&h, now + 29999));
    CHECK(PnHostHealthExpire(&h, now + 30000));        /* first quarantine: 30 s */
    CHECK(!PnHostHealthExpire(&h, now + 30001));       /* reported once */
    CHECK(!PnHostHealthQuarantined(&h, now + 30000));
    now += 30000;

    /* second run: 2 min; third and later: 10 min */
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
    CHECK(PnHostHealthQuarantined(&h, now + 119999) && !PnHostHealthExpire(&h, now + 119999));
    CHECK(PnHostHealthExpire(&h, now + 120000));
    now += 120000;
    for (unsigned round = 0; round < 3; ++round) {
        for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
        CHECK(PnHostHealthQuarantined(&h, now + 599999));
        CHECK(PnHostHealthExpire(&h, now + 600000));
        now += 600000;
    }

    /* sustained success (continuous traffic) resets the escalation to 30 s */
    for (ULONGLONG t = 0; t <= PN_HOST_HEALTHY_RESET_MS; t += 20) PnHostHealthSuccess(&h, now + t);
    now += PN_HOST_HEALTHY_RESET_MS + 20;
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
    CHECK(PnHostHealthQuarantined(&h, now + 29999) && PnHostHealthExpire(&h, now + 30000));
    now += 30000;

    /* idle gaps do not count as sustained success */
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
    CHECK(PnHostHealthExpire(&h, now + 120000));        /* second since the reset: 2 min */
    now += 120000;
    PnHostHealthSuccess(&h, now);
    PnHostHealthSuccess(&h, now + PN_HOST_HEALTHY_RESET_MS + 1);   /* two results 5 min apart */
    now += PN_HOST_HEALTHY_RESET_MS + 1;
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
    CHECK(PnHostHealthQuarantined(&h, now + 599999));    /* not forgiven: 10 min */
    CHECK(PnHostHealthExpire(&h, now + 600000));

    /* a re-registered host process starts clean but keeps its escalation */
    PnHostHealthRegistered(&h);
    CHECK(!PnHostHealthQuarantined(&h, now) && !PnHostHealthExpire(&h, now));
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&h, now);
    CHECK(PnHostHealthQuarantined(&h, now + 599999));

    /* wrap safety near the top of the clock */
    PN_HOST_HEALTH w = {};
    ULONGLONG top = ~0ull - 1000;
    for (unsigned i = 0; i < PN_HOST_FAILURE_LIMIT; ++i) PnHostHealthFailure(&w, top);
    CHECK(PnHostHealthQuarantined(&w, top + 500));
}

/* P3/P8: outstanding requests are tracked per sequence; gaps and timeouts write off
 * exactly the lost requests; round trips use the matching submit; PCM claims are bounded. */
static void TestHostLedger()
{
    PN_HOST_LEDGER l = {};
    PN_HOST_MATCH m;
    CHECK(PnHostLedgerAdd(&l, 1, 1024, 100));
    CHECK(PnHostLedgerAdd(&l, 2, 1024, 200));
    CHECK(PnHostLedgerAdd(&l, 3, 1024, 300));
    CHECK(PnHostLedgerAdd(&l, 4, 1024, 400));
    CHECK(l.Count == 4 && PnHostLedgerHeldFrames(&l) == 4096);

    m = PnHostLedgerMatch(&l, 1);
    CHECK(m.Matched && m.Lost == 0 && m.SubmitHns == 100 && m.SubmittedPcmFrames == 1024);
    CHECK(PnHostLedgerReturn(&l, 1024) == 1024 && l.Count == 3);

    /* duplicate / stale results are ignored and do not change accounting */
    m = PnHostLedgerMatch(&l, 1);
    CHECK(!m.Matched && l.Count == 3 && PnHostLedgerHeldFrames(&l) == 3072);
    m = PnHostLedgerMatch(&l, 99);
    CHECK(!m.Matched && l.Count == 3);

    /* a result for 4 proves 2 and 3 were lost: they are written off, 4 matches its own submit */
    m = PnHostLedgerMatch(&l, 4);
    CHECK(m.Matched && m.Lost == 2 && m.LostPcmFrames == 2048 && m.SubmitHns == 400);
    CHECK(l.Count == 0 && PnHostLedgerHeldFrames(&l) == 1024);
    /* a host can never return more PCM than it holds (claim of 10^9 frames) */
    CHECK(PnHostLedgerReturn(&l, 1000000000u) == 1024);
    CHECK(PnHostLedgerHeldFrames(&l) == 0);
    CHECK(PnHostLedgerReturn(&l, 5) == 0);

    /* buffering codecs (LDAC MQ) return several blocks at once: accepted while held */
    for (ULONG s = 10; s < 14; ++s) CHECK(PnHostLedgerAdd(&l, s, 256, s));
    m = PnHostLedgerMatch(&l, 10); CHECK(m.Matched && PnHostLedgerReturn(&l, 0) == 0);
    m = PnHostLedgerMatch(&l, 11); CHECK(m.Matched && PnHostLedgerReturn(&l, 0) == 0);
    m = PnHostLedgerMatch(&l, 12); CHECK(m.Matched && PnHostLedgerReturn(&l, 768) == 768);
    CHECK(PnHostLedgerHeldFrames(&l) == 256 && l.Count == 1);

    /* window is bounded; an undeliverable newest request is cancelled */
    PN_HOST_LEDGER full = {};
    for (ULONG s = 1; s <= PN_HOST_LEDGER_SLOTS; ++s) CHECK(PnHostLedgerAdd(&full, s, 128, s));
    CHECK(!PnHostLedgerAdd(&full, 100, 128, 100));
    PnHostLedgerCancelNewest(&full, 7);   /* not the newest: ignored */
    CHECK(full.Count == PN_HOST_LEDGER_SLOTS);
    PnHostLedgerCancelNewest(&full, PN_HOST_LEDGER_SLOTS);
    CHECK(full.Count == PN_HOST_LEDGER_SLOTS - 1 && PnHostLedgerHeldFrames(&full) == 128ull * (PN_HOST_LEDGER_SLOTS - 1));

    /* watchdog: only the requests older than the timeout are written off, oldest first */
    CHECK(PnHostLedgerOldestSubmit(&full) == 1);
    CHECK(PnHostLedgerDropOldest(&full) == 128 && full.Count == PN_HOST_LEDGER_SLOTS - 2);
    CHECK(PnHostLedgerOldestSubmit(&full) == 2);
    m = PnHostLedgerMatch(&full, 1);      /* the late answer of a written-off request */
    CHECK(!m.Matched && full.Count == PN_HOST_LEDGER_SLOTS - 2);
    m = PnHostLedgerMatch(&full, 2);
    CHECK(m.Matched && m.Lost == 0 && m.SubmitHns == 2);
    while (full.Count) PnHostLedgerDropOldest(&full);
    CHECK(PnHostLedgerDropOldest(&full) == 0 && PnHostLedgerOldestSubmit(&full) == 0);

    /* sequence numbers wrap */
    PN_HOST_LEDGER wrap = {};
    CHECK(PnHostLedgerAdd(&wrap, 0xffffffffu, 100, 1) && PnHostLedgerAdd(&wrap, 0, 100, 2) && PnHostLedgerAdd(&wrap, 1, 100, 3));
    m = PnHostLedgerMatch(&wrap, 1);
    CHECK(m.Matched && m.Lost == 2 && wrap.Count == 0);
}

/* P8: result metadata is bounded before it reaches the packetiser. */
static void TestHostResultBounds()
{
    CHECK(PnHostResultValid(600, 2048, 4, 4, TRUE));            /* SBC/LDAC */
    CHECK(PnHostResultValid(600, 2048, 4, 0, TRUE));            /* default header */
    CHECK(PnHostResultValid(0, 2048, 0, 0, TRUE));              /* nothing to send yet */
    CHECK(PnHostResultValid(860, 2048, 1, 0, FALSE));           /* AAC */
    CHECK(PnHostResultValid(640, 2048, 80, 0, FALSE));          /* aptX: 8 bytes per block */
    CHECK(!PnHostResultValid(2049, 2048, 1, 0, FALSE));         /* oversize */
    CHECK(!PnHostResultValid(600, 2048, 16, 0, TRUE));          /* 4-bit frame count */
    CHECK(!PnHostResultValid(600, 2048, 4, 0x105, TRUE));       /* header is one byte */
    CHECK(!PnHostResultValid(600, 2048, 4, 5, TRUE));           /* header contradicts FrameCount */
    CHECK(!PnHostResultValid(10, 2048, 11, 0, FALSE));          /* more frames than bytes */
    CHECK(!PnHostResultValid(10, 2048, 0, 0, FALSE));           /* payload without frames */
    CHECK(!PnHostResultValid(0, 2048, 3, 0, FALSE));            /* frames without payload */
    CHECK(!PnHostResultValid(10, 2048, 0xffffffffu, 0, FALSE));
}

/* P5: an AAC access unit plus LATM must fit one RTP packet of the media MTU. */
static void TestAacMtuCap()
{
    /* Sony CMT-SBT40D style MTU 895: 883 bytes after the RTP header */
    ULONG cap441 = PnAacMaxBitrateForPayload(895 - 12, 44100);
    ULONG cap48 = PnAacMaxBitrateForPayload(895 - 12, 48000);
    CHECK(cap441 > 0 && cap441 < 320000);
    CHECK((ULONGLONG)cap441 * 1024 / 44100 / 8 + PN_AAC_LATM_OVERHEAD <= 883);   /* average AU fits */
    CHECK(cap48 <= 320000 && (ULONGLONG)cap48 * 1024 / 48000 / 8 + PN_AAC_LATM_OVERHEAD <= 883);
    CHECK(PnAacMaxBitrateForPayload(1005 - 12, 44100) == 320000);   /* large MTU: sink limit applies */
    CHECK(PnAacMaxBitrateForPayload(339 - 12, 44100) < 128000);
    CHECK(PnAacMaxBitrateForPayload(PN_AAC_LATM_OVERHEAD, 44100) == 0);   /* unusable: no cap */
    CHECK(PnAacMaxBitrateForPayload(883, 0) == 0);
    for (ULONG room = PN_AAC_LATM_OVERHEAD + 1; room < 2048; room += 7) {
        for (ULONG rate : {44100u, 48000u}) {
            ULONG cap = PnAacMaxBitrateForPayload(room, rate);
            CHECK(cap <= 320000);
            CHECK((ULONGLONG)cap * 1024 / rate / 8 + PN_AAC_LATM_OVERHEAD <= room);
        }
    }
}

/* P4/P10/C4: media writes are tracked with their PCM; a write's own send time excludes
 * the time it queued behind the previous one; a stall is measured from the later of the
 * last completion and the oldest outstanding submission. */
static void TestTxLedger()
{
    const LONGLONG ms = 10000;   /* 100 ns units */
    PN_TX_LEDGER l = {};
    ULONG send = 0;
    /* three writes queued together at t=0 (an AAC AU in two fragments, then an SBC packet) */
    CHECK(PnTxLedgerPush(&l, 0, 0) && PnTxLedgerPush(&l, 1024, 0) && PnTxLedgerPush(&l, 640, 0));
    CHECK(l.Count == 3 && l.InFlightFrames == 1664);
    /* each takes 10 ms on the link: send times are 10/10/10 ms, not 10/20/30 */
    CHECK(PnTxLedgerComplete(&l, 0, 10 * ms, 500, &send) == 0 && send == 10000);
    CHECK(PnTxLedgerComplete(&l, 0, 20 * ms, 400, &send) == 1024 && send == 10000);
    CHECK(PnTxLedgerComplete(&l, 0, 30 * ms, 600, &send) == 640 && send == 10000);
    CHECK(l.Count == 0 && l.InFlightFrames == 0 && l.DeliveredFrames == 1664);
    CHECK(l.DeliveredBits == 1500 * 8 && l.BusyUs == 30000);
    /* a write submitted after an idle gap: its send time starts at its own submission */
    CHECK(PnTxLedgerPush(&l, 128, 100 * ms));
    CHECK(PnTxLedgerComplete(&l, 100 * ms, 103 * ms, 100, &send) == 128 && send == 3000);
    /* completions reported out of order never produce a negative time */
    CHECK(PnTxLedgerPush(&l, 1, 110 * ms) && PnTxLedgerPush(&l, 2, 110 * ms));
    CHECK(PnTxLedgerComplete(&l, 110 * ms, 115 * ms, 10, &send) == 1 && send == 5000);
    CHECK(PnTxLedgerComplete(&l, 110 * ms, 114 * ms, 10, &send) == 2 && send == 0);
    CHECK(PnTxLedgerComplete(&l, 110 * ms, 116 * ms, 10, &send) == 0 && l.InFlightFrames == 0);   /* unknown write */
    /* a synchronous submit failure takes back exactly the newest write */
    CHECK(PnTxLedgerPush(&l, 300, 120 * ms) && PnTxLedgerPush(&l, 700, 120 * ms));
    PnTxLedgerCancelNewest(&l);
    CHECK(l.Count == 1 && l.InFlightFrames == 300);
    PnTxLedgerCancelNewest(&l);
    PnTxLedgerCancelNewest(&l);
    CHECK(l.Count == 0 && l.InFlightFrames == 0);
    PN_TX_LEDGER full = {};
    for (ULONG i = 0; i < PN_TX_LEDGER_SLOTS; ++i) CHECK(PnTxLedgerPush(&full, i, i));
    CHECK(!PnTxLedgerPush(&full, 1, 1));

    /* C4 stall: nothing outstanding is never a stall */
    PN_TX_LEDGER s = {};
    CHECK(!PnTxLedgerStalled(&s, 1000000 * ms, 500));
    /* last completion 10 s ago, a write queued now: not stalled until 500 ms after it */
    s.LastDoneHns = 1000 * ms;
    CHECK(PnTxLedgerPush(&s, 512, 11000 * ms));
    CHECK(!PnTxLedgerStalled(&s, 11499 * ms, 500));
    CHECK(PnTxLedgerStalled(&s, 11500 * ms, 500));
    CHECK(!PnTxLedgerStalled(&s, 11500 * ms, 0));
    /* completions keep a busy queue alive */
    CHECK(PnTxLedgerPush(&s, 512, 11001 * ms));
    PnTxLedgerComplete(&s, 11000 * ms, 11400 * ms, 100, &send);
    CHECK(!PnTxLedgerStalled(&s, 11899 * ms, 500) && PnTxLedgerStalled(&s, 11900 * ms, 500));
    PnTxLedgerComplete(&s, 11001 * ms, 11950 * ms, 100, &send);
    CHECK(!PnTxLedgerStalled(&s, 20000 * ms, 500));
}

/* C3/P4: exact pipeline accounting. A block moving from the ring to the codec host, into
 * the result queue and into (fragmented) writes keeps the latency unchanged; it falls
 * only when the receiver has the audio. LDAC MQ packs 3 blocks (768 frames) per write. */
struct PipelineModel {
    ULONGLONG ring;            /* frames in the ring */
    PN_HOST_LEDGER host;
    ULONGLONG queued;          /* frames in encoded results */
    PN_TX_LEDGER tx;
};
static ULONG PipelineUs(const PipelineModel &m, ULONG rate)
{
    return PnPipelineLatencyUs(m.ring, rate, PnHostLedgerHeldFrames(&m.host) + m.queued + m.tx.InFlightFrames, rate, 0);
}
static void TestPipelineLatency()
{
    const ULONG rate = 48000;
    for (ULONG blocks : {1u, 3u}) {   /* AAC: one 1024-frame AU per result; LDAC MQ: 3 x 256 per packet */
        ULONG block = blocks == 1 ? 1024 : 256;
        PipelineModel m = {};
        m.ring = 9600;   /* 200 ms */
        const ULONG start = PipelineUs(m, rate);
        CHECK(start == 200000);
        ULONG seq = 1, maxDiff = 0, finalUs = 0;
        for (unsigned round = 0; round < 6; ++round) {
            /* submit `blocks` blocks to the host */
            for (ULONG b = 0; b < blocks; ++b) {
                m.ring -= block;
                CHECK(PnHostLedgerAdd(&m.host, seq++, block, 0));
                ULONG us = PipelineUs(m, rate);
                maxDiff = PN_MAX(maxDiff, us > start ? us - start : start - us);
            }
            /* results: buffering codecs return nothing for the first blocks, then all PCM */
            ULONGLONG got = 0;
            for (ULONG b = 0; b < blocks; ++b) {
                PN_HOST_MATCH hm = PnHostLedgerMatch(&m.host, seq - blocks + b);
                CHECK(hm.Matched);
                got += PnHostLedgerReturn(&m.host, b + 1 == blocks ? block * blocks : 0);
            }
            m.queued += got;
            maxDiff = PN_MAX(maxDiff, (ULONG)(PipelineUs(m, rate) > start ? PipelineUs(m, rate) - start : start - PipelineUs(m, rate)));
            /* the packetiser sends it, AAC as two fragments with the PCM on the last */
            m.queued -= got;
            CHECK(PnTxLedgerPush(&m.tx, 0, 0) && PnTxLedgerPush(&m.tx, (ULONG)got, 0));
            ULONG us = PipelineUs(m, rate);
            maxDiff = PN_MAX(maxDiff, us > start ? us - start : start - us);
            /* new PCM arrives meanwhile; the write completes */
            m.ring += got;
            ULONG send;
            PnTxLedgerComplete(&m.tx, 0, 1, 100, &send);
            PnTxLedgerComplete(&m.tx, 0, 2, 100, &send);
            finalUs = PipelineUs(m, rate);
        }
        CHECK(maxDiff <= 1);           /* no jump when a block changes hands (1 us rounding) */
        CHECK(finalUs == start);       /* steady state: as much arrived as was delivered */
        CHECK(m.tx.InFlightFrames == 0 && PnHostLedgerHeldFrames(&m.host) == 0);
    }
    /* send time and the codec rate (a 96 kHz engine feeding a 48 kHz codec) add up */
    CHECK(PnPipelineLatencyUs(9600, 96000, 4800, 48000, 1500) == 100000 + 100000 + 1500);
    CHECK(PnPipelineLatencyUs(0, 0, 0, 0, 7) == 7);
    CHECK(PnPipelineLatencyUs(~0ull, 1, 0, 48000, 0) == 0xffffffffu);
}

/* P6: the latency ceiling stays below the ring; trims keep both sides of a crossfade. */
static void TestLatencyGuard()
{
    ULONG t, m;
    PnEffectiveLatencyMs(60, 250, 600, &t, &m);
    CHECK(t == 60 && m == 250);                     /* defaults unchanged */
    PnEffectiveLatencyMs(400, 1000, 600, &t, &m);   /* regcfg allows 1000; the ring holds 600 */
    CHECK(m == 600 - PN_RING_HEADROOM_MS && t == 400 && m * 1000 < 600000);
    PnEffectiveLatencyMs(400, 420, 600, &t, &m);
    CHECK(m == 420 && t == 400);
    PnEffectiveLatencyMs(300, 250, 600, &t, &m);    /* target above the ceiling */
    CHECK(m == 250 && t == 230);
    PnEffectiveLatencyMs(0, 0, 600, &t, &m);
    CHECK(m == 40 && t == 0);
    PnEffectiveLatencyMs(0xffffffffu, 0xffffffffu, 600, &t, &m);
    CHECK(m == 500 && t == 480);
    PnEffectiveLatencyMs(60, 250, 0, &t, &m);
    CHECK(m == 40 && t == 20);

    ULONG fade;
    /* 250 ms of 48 kHz stereo S16 trimmed to 60 ms with a 4 ms (192 frame) fade */
    ULONG drop = PnTrimPlan(48000, 11520, 4, 192, 8192, &fade);
    CHECK(drop == 48000 - 11520 && fade == 192);
    drop = PnTrimPlan(48002, 11520, 4, 192, 8192, &fade);           /* frame aligned */
    CHECK(drop % 4 == 0 && drop <= 48002 - 11520 && 48002 - drop - 11520 < 4);
    CHECK(PnTrimPlan(1000, 1000, 4, 192, 8192, &fade) == 0 && fade == 0);
    CHECK(PnTrimPlan(1000, 2000, 4, 192, 8192, &fade) == 0 && fade == 0);
    CHECK(PnTrimPlan(1000, 0, 0, 192, 8192, &fade) == 0 && fade == 0);
    CHECK(PnTrimPlan(1000, 800, 4, 192, 8192, &fade) == 200 && fade == 50);     /* short drop */
    CHECK(PnTrimPlan(4000, 40, 4, 192, 8192, &fade) == 3960 && fade == 10);     /* short keep */
    CHECK(PnTrimPlan(400000, 100, 8, 768, 8192, &fade) == 399896 && fade == 13);
    CHECK(PnTrimPlan(400000, 10000, 8, 768, 8192, &fade) == 390000 && fade == 768);
    CHECK(PnTrimPlan(400000, 10000, 8, 768, 4096, &fade) == 390000 && fade == 512);   /* buffer cap */
    CHECK(PnTrimPlan(400000, 10000, 8, 768, 0, &fade) == 390000 && fade == 0);
}

/* P6: volume and mute ramp across a block; with no change the output is bit-exact. */
static void TestGainRamp()
{
    const ULONG frames = 480;
    std::vector<SHORT> in(frames * 2, 16000), out(frames * 2);
    PN_PCM_STATE st = {};
    ULONG bytes = frames * 4;
    /* first block: no ramp from an unknown gain */
    CHECK(PnConvertPcmEx(&st, (UCHAR*)in.data(), bytes, 2, 16, 16, FALSE, (UCHAR*)out.data(), bytes, PNA2DP_FMT_S16, 2, 32768) == bytes);
    CHECK(out[0] == 16000 && out[frames * 2 - 1] == 16000);
    /* mute: ramps down across the block instead of stepping to silence */
    CHECK(PnConvertPcmEx(&st, (UCHAR*)in.data(), bytes, 2, 16, 16, FALSE, (UCHAR*)out.data(), bytes, PNA2DP_FMT_S16, 2, 0) == bytes);
    int maxStep = std::abs(16000 - out[0]);
    for (ULONG f = 1; f < frames; ++f) {
        CHECK(out[f * 2] <= out[(f - 1) * 2] && out[f * 2] == out[f * 2 + 1]);
        maxStep = PN_MAX(maxStep, std::abs(out[f * 2] - out[(f - 1) * 2]));
    }
    CHECK(out[0] > 15900 && out[(frames - 1) * 2] == 0);
    CHECK(maxStep <= 16000 / (int)frames + 1);
    CHECK(PnConvertPcmEx(&st, (UCHAR*)in.data(), bytes, 2, 16, 16, FALSE, (UCHAR*)out.data(), bytes, PNA2DP_FMT_S16, 2, 0) == bytes);
    bool silent = true;
    for (SHORT v : out) silent = silent && v == 0;
    CHECK(silent);
    /* unmute to -6 dB ramps up and ends exactly at the steady value */
    ULONG half = PnVolumeGainQ15(-6 * 65536);
    PnConvertPcmEx(&st, (UCHAR*)in.data(), bytes, 2, 16, 16, FALSE, (UCHAR*)out.data(), bytes, PNA2DP_FMT_S16, 2, half);
    SHORT steady[2] = {};
    PnConvertPcm((UCHAR*)in.data(), 4, 2, 16, 16, FALSE, (UCHAR*)steady, 4, PNA2DP_FMT_S16, 2, half);
    CHECK(out[0] < 100 && out[(frames - 1) * 2] == steady[0]);
    for (ULONG f = 1; f < frames; ++f) CHECK(out[f * 2] >= out[(f - 1) * 2]);
    /* an empty block does not consume the ramp */
    CHECK(PnConvertPcmEx(&st, (UCHAR*)in.data(), 0, 2, 16, 16, FALSE, (UCHAR*)out.data(), bytes, PNA2DP_FMT_S16, 2, 0) == 0);
    CHECK(st.GainQ15 == half);

    /* constant gain with state is bit-exact to the stateless path (every format, mono mixdown) */
    ULONG random = 0x1234567;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    std::vector<ULONG> src(256);
    for (ULONG &v : src) v = next();
    for (ULONG bits : {16u, 24u, 32u}) for (ULONG fmt : {(ULONG)PNA2DP_FMT_S16, (ULONG)PNA2DP_FMT_S24, (ULONG)PNA2DP_FMT_S32})
    for (ULONG outCh : {1u, 2u}) for (ULONG gain : {0u, 1u, 9235u, 32768u}) {
        std::vector<UCHAR> a(2048), b(2048);
        PN_PCM_STATE g = {};
        g.GainValid = TRUE; g.GainQ15 = gain;
        ULONG na = PnConvertPcm((UCHAR*)src.data(), 1020, 2, bits, bits, FALSE, a.data(), 2048, fmt, outCh, gain);
        ULONG nb = PnConvertPcmEx(&g, (UCHAR*)src.data(), 1020, 2, bits, bits, FALSE, b.data(), 2048, fmt, outCh, gain);
        CHECK(na == nb && na && memcmp(a.data(), b.data(), na) == 0);
    }
}

/* P6: a latency trim crossfades from the audio that would have followed the cut into the
 * kept audio, across block boundaries, instead of jumping. */
static void TestTrimCrossfade()
{
    const ULONG fade = 192;   /* 4 ms at 48 kHz */
    /* constant +12000 cut to constant -12000 */
    std::vector<SHORT> outgoing(fade * 2, 12000), incoming(400 * 2, -12000), out(400 * 2);
    PN_PCM_STATE st = {};
    st.FadeOut = (const UCHAR*)outgoing.data(); st.FadeFrames = fade;
    CHECK(PnConvertPcmEx(&st, (UCHAR*)incoming.data(), 128 * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), 128 * 4, PNA2DP_FMT_S16, 2, 32768) == 128 * 4);
    CHECK(st.FadePos == 128 && st.FadeFrames == fade);
    CHECK(PnConvertPcmEx(&st, (UCHAR*)incoming.data(), 272 * 4, 2, 16, 16, FALSE, (UCHAR*)(out.data() + 128 * 2), 272 * 4, PNA2DP_FMT_S16, 2, 32768) == 272 * 4);
    CHECK(st.FadeFrames == 0 && st.FadePos == 0 && !st.FadeOut);
    int maxStep = std::abs(12000 - out[0]);
    for (ULONG f = 1; f < 400; ++f) maxStep = PN_MAX(maxStep, std::abs(out[f * 2] - out[(f - 1) * 2]));
    CHECK(out[0] > 11800);
    CHECK(maxStep <= 24000 / (int)(fade + 1) + 1);   /* abrupt cut: 24000 */
    CHECK(out[fade * 2] == -12000 && out[399 * 2 + 1] == -12000);

    /* a 1 kHz sine trimmed at an arbitrary phase: no step beyond the sine's own slope */
    const double pi = 3.14159265358979323846;
    std::vector<SHORT> ring(48000 * 2);
    for (ULONG f = 0; f < 48000; ++f) ring[f * 2] = ring[f * 2 + 1] = (SHORT)std::lround(16000.0 * std::sin(2 * pi * 1000.0 * f / 48000.0));
    ULONG played = 1000, keep = 2880 * 4;    /* 60 ms kept */
    ULONG avail = (48000 - played) * 4, fadeFrames = 0;
    ULONG drop = PnTrimPlan(avail, keep, 4, fade, 8192, &fadeFrames);
    CHECK(drop && fadeFrames == fade);
    ULONG cut = played + drop / 4 + 13;      /* shift so the phases do not line up */
    PN_PCM_STATE s2 = {};
    s2.FadeOut = (const UCHAR*)&ring[played * 2]; s2.FadeFrames = fadeFrames;
    std::vector<SHORT> after(1024 * 2);
    PnConvertPcmEx(&s2, (UCHAR*)&ring[cut * 2], 1024 * 4, 2, 16, 16, FALSE, (UCHAR*)after.data(), 1024 * 4, PNA2DP_FMT_S16, 2, 32768);
    int worst = std::abs(after[0] - ring[(played - 1) * 2]);   /* last sample played before the cut */
    for (ULONG f = 1; f < 1024; ++f) worst = PN_MAX(worst, std::abs(after[f * 2] - after[(f - 1) * 2]));
    int natural = (int)std::ceil(16000.0 * 2 * pi * 1000.0 / 48000.0);   /* ~2095 */
    CHECK(worst <= natural + 32000 / (int)(fade + 1) + 2);
    CHECK(!PnPcmFadePending(&s2));

    /* a stall trims again before anything was converted (Low latency: every ~90 ms): the
     * pending crossfade keeps its source, which continues what was played last (PumpTrim) */
    PN_PCM_STATE s3 = {};
    s3.FadeOut = (const UCHAR*)&ring[played * 2]; s3.FadeFrames = fadeFrames;   /* first trim */
    CHECK(PnPcmFadePending(&s3));
    ULONG oldSource = played + 2999;         /* head of the second drop: opposite phase to the last sample played */
    ULONG cut2 = played + 6000 + 12;         /* kept audio after the second trim */
    std::vector<SHORT> after2(1024 * 2);
    PnConvertPcmEx(&s3, (UCHAR*)&ring[cut2 * 2], 1024 * 4, 2, 16, 16, FALSE, (UCHAR*)after2.data(), 1024 * 4, PNA2DP_FMT_S16, 2, 32768);
    worst = std::abs(after2[0] - ring[(played - 1) * 2]);
    for (ULONG f = 1; f < 1024; ++f) worst = PN_MAX(worst, std::abs(after2[f * 2] - after2[(f - 1) * 2]));
    CHECK(worst <= natural + 32000 / (int)(fade + 1) + 2);
    /* 0.1.0.21 replaced the source with the head of the second drop (audio that followed
     * dropped audio): the first sample steps by most of the amplitude */
    PN_PCM_STATE old = {};
    old.FadeOut = (const UCHAR*)&ring[oldSource * 2]; old.FadeFrames = fadeFrames;
    PnConvertPcmEx(&old, (UCHAR*)&ring[cut2 * 2], 1024 * 4, 2, 16, 16, FALSE, (UCHAR*)after2.data(), 1024 * 4, PNA2DP_FMT_S16, 2, 32768);
    CHECK(std::abs(after2[0] - ring[(played - 1) * 2]) > 4 * natural);
}

/* P10: link congestion only; trims caused by the link reduce at once; reductions jump
 * to ~0.8x the measured throughput; host starvation is not congestion. */
static PN_ABR_SAMPLE AbrSample(ULONG queueFull, ULONG trims, ULONG backlogUs, ULONG slow = 0)
{
    PN_ABR_SAMPLE s = {};
    s.ElapsedMs = 250; s.Completed = 20; s.Slow = slow; s.QueueFull = queueFull; s.Trims = trims;
    s.BacklogPeakUs = backlogUs; s.TargetLatencyUs = 60000;
    s.DeliveredMediaUs = 250000; s.DeliveredBits = 80000; s.BusyUs = 100000;
    return s;
}
static void TestAbrSignals()
{
    /* host starvation: the ring backs up and trims while the TX queue has room */
    PN_ABR_SAMPLE s = AbrSample(0, 1, 400000);
    CHECK(PnAbrReasons(&s) == 0);
    PN_QUALITY_WINDOW w = {};
    ULONG reasons = 0, ladder[5] = {328000, 280000, 240000, 200000, 160000};
    for (unsigned i = 0; i < 40; ++i) CHECK(PnAbrDecide(&w, &s, ladder, TRUE, 0, 4, &reasons) == 0 && reasons == 0);
    /* the same backlog with a full TX queue is congestion */
    s = AbrSample(3, 0, 400000);
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_BACKLOG);
    s = AbrSample(3, 0, 50000);
    CHECK(PnAbrReasons(&s) == 0);
    /* delivered media time below real time while the queue is full */
    s = AbrSample(3, 0, 0); s.DeliveredMediaUs = 200000;
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_SLOW);
    s.QueueFull = 0;
    CHECK(PnAbrReasons(&s) == 0);
    s = AbrSample(0, 0, 0, 2);
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_SLOW);
    s = AbrSample(0, 0, 0, 1);
    CHECK(PnAbrReasons(&s) == 0);
    s = AbrSample(0, 0, 0); s.Failed = 1; s.Pressure = 1;
    CHECK(PnAbrReasons(&s) == (PNA2DP_QUALITY_REASON_FAILED | PNA2DP_QUALITY_REASON_PRESSURE));

    /* budget: 80000 bits in 100 ms busy = 800 kbps sustained -> 640 kbps */
    s = AbrSample(3, 0, 0);
    CHECK(PnAbrBudgetBps(&s) == 640000);
    s.Completed = 3;
    CHECK(PnAbrBudgetBps(&s) == 0);
    s = AbrSample(3, 0, 0); s.BusyUs = 49999;
    CHECK(PnAbrBudgetBps(&s) == 0);
    CHECK(PnAbrLevelForBudget(ladder, 4, 0, 0) == 1);
    CHECK(PnAbrLevelForBudget(ladder, 4, 0, 400000) == 1);      /* current rate fits: one rung */
    CHECK(PnAbrLevelForBudget(ladder, 4, 0, 210000) == 3);
    CHECK(PnAbrLevelForBudget(ladder, 4, 1, 100000) == 4);      /* below the floor: the floor */
    CHECK(PnAbrLevelForBudget(ladder, 4, 4, 100000) == 4);

    /* a trim with the TX queue full reduces at once, straight to the sustainable rung */
    w = {};
    s = AbrSample(2, 1, 250000); s.DeliveredBits = 40000; s.BusyUs = 250000;   /* 160 kbps -> 128 kbps budget */
    int change = PnAbrDecide(&w, &s, ladder, TRUE, 0, 4, &reasons);
    CHECK(change == 4 && (reasons & PNA2DP_QUALITY_REASON_BACKLOG));
    /* disabled ABR, or already at the floor: nothing */
    w = {};
    CHECK(PnAbrDecide(&w, &s, ladder, FALSE, 0, 4, &reasons) == 0);
    CHECK(PnAbrDecide(&w, &s, ladder, TRUE, 4, 4, &reasons) == 0);
    /* congestion without a trim keeps the 1 s confirmation and the 2 s spacing (a new
     * stream's window starts at 0 ms since change), then jumps */
    w = {};
    s = AbrSample(3, 0, 300000); s.DeliveredBits = 50000; s.BusyUs = 250000;   /* 200 kbps -> 160 kbps */
    for (unsigned i = 0; i < 7; ++i) CHECK(PnAbrDecide(&w, &s, ladder, TRUE, 0, 4, &reasons) == 0);
    CHECK(PnAbrDecide(&w, &s, ladder, TRUE, 0, 4, &reasons) == 4);
    /* no ladder: single rungs */
    w = {};
    for (unsigned i = 0; i < 7; ++i) PnAbrDecide(&w, &s, NULL, TRUE, 0, 4, &reasons);
    CHECK(PnAbrDecide(&w, &s, NULL, TRUE, 0, 4, &reasons) == 1);
    /* recovery cadence after a jump is unchanged: 30 s of clean traffic per rung first */
    s = AbrSample(0, 0, 0);
    int up = 0; unsigned ticks = 0;
    for (; ticks < 200 && up == 0; ++ticks) up = PnAbrDecide(&w, &s, ladder, TRUE, 4, 4, &reasons);
    CHECK(up == -1 && ticks == 120);
    /* an urgent reduction right after a recovery step earns the 60 s penalty */
    s = AbrSample(2, 1, 250000);
    CHECK(PnAbrDecide(&w, &s, ladder, TRUE, 3, 4, &reasons) >= 1 && PnQualityRecoveryTarget(&w) == 60000);
}

/* P10: a link at half the needed throughput. The old controller (1 s of congestion, then
 * one ~15% rung per 2 s) trimmed repeatedly for seconds; now one trim settles it. */
static unsigned SimulateHalfThroughput(bool legacy, ULONG *settledMs, ULONG *finalBps, ULONG *capBps)
{
    UCHAR offer[] = {0x21, 0x15, 2, 53};   /* SBC 44.1 kHz joint stereo, bitpool 53 */
    A2DP_CODEC_CONFIG cfg = {};
    if (!A2dpParseConfig(A2DP_CODEC_SBC, offer, sizeof(offer), &cfg)) return 999;
    cfg.SbcBitpool = cfg.SbcMaxBitpool;
    ULONG ladder[25], maxLevel = 24;
    for (ULONG i = 0; i <= 24; ++i) ladder[i] = A2dpQualityAtLevel(&cfg, i, 0, NULL);
    while (maxLevel && ladder[maxLevel] == ladder[maxLevel - 1]) maxLevel--;
    const double cap = ladder[0] / 2.0;
    *capBps = (ULONG)cap;
    PN_QUALITY_WINDOW w = {};
    ULONG level = 0;
    double ringMs = 60;
    unsigned trims = 0;
    *settledMs = 0;
    for (ULONG t = 250; t <= 20000; t += 250) {
        double rate = ladder[level];
        bool saturated = rate > cap;
        if (saturated) ringMs += 250.0 * (1.0 - cap / rate);
        ULONG trimmed = 0;
        if (ringMs > 250) { ringMs = 60; trimmed = 1; ++trims; }
        PN_ABR_SAMPLE s = {};
        s.ElapsedMs = 250; s.Completed = 18; s.TargetLatencyUs = 60000;
        s.Slow = saturated ? 18 : 0;
        s.QueueFull = saturated ? 10 : 0;
        s.Trims = trimmed;
        s.BacklogPeakUs = saturated ? (ULONG)(ringMs * 1000) : 0;
        s.DeliveredMediaUs = (ULONGLONG)(250000.0 * (saturated ? cap / rate : 1.0));
        s.DeliveredBits = (ULONGLONG)((saturated ? cap : rate) / 4);
        s.BusyUs = (ULONGLONG)(saturated ? 250000.0 : 250000.0 * rate / cap);
        int change;
        if (legacy) {
            /* pump.cpp before this change: trims counted as pressure, one rung at a time */
            BOOLEAN bad = trimmed || s.BacklogPeakUs > s.TargetLatencyUs || (s.Slow >= 2 && s.Slow * 10 >= s.Completed);
            change = PnQualityObserve(&w, 250, TRUE, bad, TRUE, level, maxLevel);
        } else {
            ULONG reasons;
            change = PnAbrDecide(&w, &s, ladder, TRUE, level, maxLevel, &reasons);
        }
        if (change > 0) level = PN_MIN(level + (ULONG)change, maxLevel);
        else if (change < 0) level--;
        if (!*settledMs && ladder[level] <= cap) *settledMs = t;
    }
    *finalBps = ladder[level];
    return trims;
}
static void TestAbrHalfThroughput()
{
    ULONG settled = 0, finalBps = 0, cap = 0;
    unsigned trims = SimulateHalfThroughput(false, &settled, &finalBps, &cap);
    CHECK(trims <= 1);
    CHECK(settled && settled <= 1000);
    CHECK(finalBps <= cap && finalBps >= cap / 2);   /* ~0.8x, not the floor */
    ULONG legacySettled = 0, legacyFinal = 0;
    unsigned legacyTrims = SimulateHalfThroughput(true, &legacySettled, &legacyFinal, &cap);
    printf("ABR at half throughput (%u bps link): %u trim(s), settled after %u ms at %u bps; "
           "previous controller: %u trims, settled after %u ms\n", cap, trims, settled, finalBps, legacyTrims, legacySettled);
}

/* P13: a WaveRT write wakes the pump only when it completes an encoder input block. */
static void TestPumpWakeups()
{
    /* 1 ms timer writes of 48 frames (S16 stereo), SBC blocks of 8 x 128 frames */
    const ULONG write = 48 * 4, block = 1024 * 4;
    ULONG avail = 0, wakes = 0;
    for (unsigned ms = 0; ms < 1000; ++ms) {
        if (PnPcmWriteShouldWake(avail, write, block, FALSE)) {
            ++wakes;
            avail += write;
            while (avail >= block) avail -= block;   /* the pump takes whole blocks */
        } else {
            avail += write;
        }
    }
    CHECK(wakes >= 46 && wakes <= 47);   /* 48000 / 1024 blocks per second, not 1000 */
    CHECK(PnPcmWriteShouldWake(0, 10, 0, FALSE));            /* unknown block size */
    CHECK(PnPcmWriteShouldWake(9000, 10, 4096, TRUE));       /* overrun */
    CHECK(!PnPcmWriteShouldWake(5000, 10, 4096, FALSE));     /* already above: completions wake */
    CHECK(PnPcmWriteShouldWake(4095, 1, 4096, FALSE));
    CHECK(!PnPcmWriteShouldWake(4000, 95, 4096, FALSE));
    CHECK(PnPcmWriteShouldWake(4000, 0xffffffffu, 4096, FALSE));
}

/* ---- Step 3: SBC packing and XQ (P9), dither and 24-bit SBC input (P11), 2:1 decimator (P12/C8) ---- */

/* the frames-per-packet rule before P9, for equivalence checks */
static ULONG OldSbcFramesPerPacket(ULONG frameBytes, ULONG frameUs, ULONG targetMs, ULONG room)
{
    ULONG fitMtu = room / PN_MAX(frameBytes, 1u);
    ULONG fitTime = PN_MAX((targetMs * 1000u / 3u) / PN_MAX(frameUs, 1u), 1u);
    return PN_MAX(1u, PN_MIN(PN_MIN(fitMtu, fitTime), 15u));
}

/* the SBC ABR ladder before P9 */
static ULONG OldSbcLadder(ULONG maxbp, ULONG minbp, ULONG level)
{
    ULONG bp = maxbp;
    for (ULONG i = 0; i < level; i++) {
        ULONG step = PN_MAX(bp / 7, 2u);
        if (bp <= minbp + step) { bp = minbp; break; }
        bp -= step;
    }
    return bp;
}

static ULONG SbcLen(ULONG rate, ULONG mode, ULONG bp)
{
    sbc_params p;
    p.sample_rate = rate; p.channel_mode = (sbc_u8)mode; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = (sbc_u8)bp;
    return sbc_frame_length(&p);
}

/* P9: frames per packet fit one EDR baseband packet; SBC XQ; the 2-DH5 ABR rung. */
static void TestSbcPacking()
{
    const ULONG hdr = 13;   /* RTP 12 + SBC media payload header 1 */
    const ULONGLONG edr = (1ull << 25) | (1ull << 26) | (1ull << 39) | (1ull << 40) | 3;
    CHECK(PnBasebandSduBytes(FALSE, edr) == 0 && PnBasebandSduBytes(TRUE, 0) == 0);
    CHECK(PnBasebandSduBytes(TRUE, edr) == 1017 && PN_SDU_3DH5 == 1017);
    CHECK(PnBasebandSduBytes(TRUE, edr & ~(1ull << 26)) == 675 && PN_SDU_2DH5 == 675);
    CHECK(PnBasebandSduBytes(TRUE, (edr & ~(1ull << 40))) == 548);
    CHECK(PnBasebandSduBytes(TRUE, (edr & ~((1ull << 40) | (1ull << 26)))) == 363);
    CHECK(PnBasebandSduBytes(TRUE, 3) == 335 && PnBasebandSduBytes(TRUE, 1) == 179 && PnBasebandSduBytes(TRUE, 1ull << 5) == 23);

    /* the arithmetic of the review: joint 53 = 119-byte frames, 6 per packet = 727 > 675
     * (2-DH5); dual 53 = 224-byte frames, 3 per packet = 685 > 675 */
    CHECK(SbcLen(44100, SBC_MODE_JOINT, 53) == 119 && hdr + 6 * 119 == 727);
    CHECK(SbcLen(44100, SBC_MODE_DUAL, 53) == 224 && hdr + 3 * 224 == 685);
    const ULONG frameUs = 128u * 1000000u / 44100u, room895 = 895 - hdr;
    /* the Sony case (joint 53, MTU 895, 60 ms): 6 frames without radio data and with a
     * 3-DH5 radio - unchanged - and 5 (608 bytes) on a 2-DH5-only radio */
    CHECK(PnSbcPacking(119, frameUs, 60, room895, hdr, 0) == 6);
    CHECK(PnSbcPacking(119, frameUs, 60, room895, hdr, 1017) == 6);
    CHECK(PnSbcPacking(119, frameUs, 60, room895, hdr, 675) == 5 && hdr + 5 * 119 <= 675);
    /* dual 53 on 2-DH5: 2 frames (one 2-DH5, 6 slots) cost 3 slots per frame, 3 frames
     * (2-DH5 + 2-DH1, 8 slots) 2.7: the MTU rule stays (review round 2 #5) */
    RED_R2("R2-5", PnSbcPacking(224, frameUs, 60, room895, hdr, 675) == 3);
    CHECK(PnSbcPacking(224, frameUs, 60, room895, hdr, 1017) == 3 && hdr + 3 * 224 <= 1017);
    /* a frame bigger than the baseband packet: the SDU limit cannot help, MTU rule applies */
    CHECK(PnSbcPacking(513, frameUs, 60, 2048, hdr, 335) == 3);
    /* with no radio data the result is the previous rule, everywhere */
    unsigned same = 0, total = 0;
    for (ULONG fb = 20; fb <= 520; fb += 7) for (ULONG us = 500; us <= 6000; us += 550)
        for (ULONG t : {20u, 30u, 60u, 120u, 400u}) for (ULONG room : {35u, 300u, 660u, 882u, 1004u, 2035u}) {
            ++total;
            same += PnSbcPacking(fb, us, t, room, hdr, 0) == OldSbcFramesPerPacket(fb, us, t, room);
        }
    CHECK(same == total);
    /* with a known radio a packing is at most the MTU rule's count and costs no more slots
     * per frame than any count up to it, including the one that fits a baseband packet (the
     * ABR repacks with this rule at every bitpool: findings #6/#8) */
    for (ULONG sdu : {335u, 363u, 548u, 675u, 1017u}) for (ULONG fb = 20; fb <= 520; fb += 3) {
        ULONG n = PnSbcPacking(fb, frameUs, 60, 2035, hdr, sdu), old = PnSbcPacking(fb, frameUs, 60, 2035, hdr, 0);
        ULONG fit = (sdu - hdr) / fb;
        CHECK(n >= 1 && n <= 15 && n <= old);
        BOOLEAN cheapest = TRUE;
        for (ULONG k = 1; k <= old; ++k) cheapest = cheapest && PnSduSlots(sdu, hdr + n * fb) * k <= PnSduSlots(sdu, hdr + k * fb) * n;
        CHECK(cheapest);
        if (fit && fit < n) CHECK(PnSduSlots(sdu, hdr + fit * fb) * n >= PnSduSlots(sdu, hdr + n * fb) * fit);
    }

    /* dual channel frames: 38 = 164 bytes, 4 in a 2-DH5 (669 bytes), 452 kbps at 44.1 kHz;
     * 47 = 200 bytes, 5 in a 3-DH5 (1013 bytes), 551 kbps */
    CHECK(SbcLen(44100, SBC_MODE_DUAL, 38) == 164 && SbcLen(44100, SBC_MODE_DUAL, 47) == 200);
    {
        sbc_params p; p.sample_rate = 44100; p.channel_mode = SBC_MODE_DUAL; p.allocation = 0; p.subbands = 8; p.blocks = 16;
        p.bitpool = 38; CHECK(sbc_bitrate(&p) == 452025);
        p.bitpool = 47; CHECK(sbc_bitrate(&p) == 551250);
    }
    CHECK(PnSbcPacking(164, frameUs, 60, room895, hdr, 675) == 4 && hdr + 4 * 164 == 669);
    CHECK(PnSbcPacking(200, frameUs, 60, 1021 - hdr, hdr, 1017) == 5 && hdr + 5 * 200 == 1013);
    CHECK(PnSbcPacking(200, frameUs, 60, room895, hdr, 1017) == 4);   /* MTU 895: 813 bytes, one 3-DH5 */

    /* XQ selection: dual channel, 16 blocks, 8 subbands at the bitpool plain dual channel
     * gets (known issue: the fixed 47 cap cost the Sony 600 instead of 617/672 kbps) */
    {
        UCHAR sonyCaps[] = {0x3f, 0xff, 2, 53};
        A2DP_CODEC_CAPS caps; A2DP_CODEC_CONFIG cfg, dual;
        PNA2DP_CONFIG pref = {};
        CHECK(A2dpParseCaps(A2DP_CODEC_SBC, sonyCaps, sizeof(sonyCaps), &caps, NULL, NULL));
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg) && cfg.ChannelMode == PNA2DP_CHMODE_JOINT && cfg.SbcMaxBitpool == 53);
        pref.Flags = PNA2DP_CFG_DUAL_CHANNEL_PREFER;
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &dual) && dual.ChannelMode == PNA2DP_CHMODE_DUAL && dual.SbcMaxBitpool == 53);
        pref.Flags = PNA2DP_CFG_SBC_XQ;
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg));
        CHECK(cfg.ChannelMode == PNA2DP_CHMODE_DUAL && cfg.SbcBlocks == 16 && cfg.SbcSubbands == 8);
        RED_R2("XQ", cfg.SbcMaxBitpool == 53 && cfg.SbcBitpool == 53 && cfg.BitrateBps == 617400 && cfg.BitrateBps == dual.BitrateBps);
        CHECK(cfg.SbcFitBitpool == 0);
        pref.Sbc.MaxBitpool = 60; pref.Sbc.AllowBitpoolOverride = 1;     /* as plain dual channel: the user's explicit override */
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg) && cfg.SbcMaxBitpool == 60);
        pref.Sbc = PNA2DP_SBC_PARAMS();
        UCHAR small[] = {0x3f, 0xff, 2, 35};
        CHECK(A2dpParseCaps(A2DP_CODEC_SBC, small, sizeof(small), &caps, NULL, NULL));
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg) && cfg.ChannelMode == PNA2DP_CHMODE_DUAL && cfg.SbcMaxBitpool == 35);
        UCHAR noDual[] = {0x3b, 0xff, 2, 53};   /* mono, stereo, joint */
        CHECK(A2dpParseCaps(A2DP_CODEC_SBC, noDual, sizeof(noDual), &caps, NULL, NULL));
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg) && cfg.ChannelMode == PNA2DP_CHMODE_JOINT && cfg.SbcMaxBitpool == 53);

        /* the "fits 2-DH5" rung: joint 53 x 6 frames -> 48 (667 bytes), XQ 47 x 4 -> 38, x 5 -> 30 */
        CHECK(A2dpParseCaps(A2DP_CODEC_SBC, sonyCaps, sizeof(sonyCaps), &caps, NULL, NULL));
        pref.Flags = 0;
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg));
        CHECK(A2dpSbcBitpoolForPacket(&cfg, 6, hdr, 675) == 48 && hdr + 6 * SbcLen(44100, SBC_MODE_JOINT, 48) <= 675 &&
              hdr + 6 * SbcLen(44100, SBC_MODE_JOINT, 49) > 675);
        CHECK(A2dpSbcBitpoolForPacket(&cfg, 5, hdr, 675) == 53);
        CHECK(A2dpSbcBitpoolForPacket(&cfg, 6, hdr, 13) == 0 && A2dpSbcBitpoolForPacket(&cfg, 0, hdr, 675) == 0);
        A2DP_CODEC_CONFIG ladder = cfg, at;
        ladder.SbcFitBitpool = 48;
        CHECK(A2dpQualityAtLevel(&ladder, 0, 0, &at) == cfg.BitrateBps && at.SbcBitpool == 53);
        A2dpQualityAtLevel(&ladder, 1, 0, &at); CHECK(at.SbcBitpool == 48);
        A2dpQualityAtLevel(&ladder, 2, 0, &at); CHECK(at.SbcBitpool == 46);   /* the regular rungs follow */
        pref.Flags = PNA2DP_CFG_SBC_XQ;
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &cfg));
        CHECK(A2dpSbcBitpoolForPacket(&cfg, 4, hdr, 675) == 38 && A2dpSbcBitpoolForPacket(&cfg, 5, hdr, 675) == 30);
    }
    /* the ladder without a fit rung is the previous ladder; with one it is inserted once */
    for (ULONG maxbp = 2; maxbp <= 128; ++maxbp) for (ULONG minbp : {2u, 10u, 35u}) {
        if (minbp > maxbp) continue;
        ULONG prev = maxbp;
        for (ULONG level = 0; level <= 24; ++level) {
            CHECK(PnSbcLadderBitpool(maxbp, minbp, 0, level) == OldSbcLadder(maxbp, minbp, level));
            ULONG fit = (maxbp + minbp) / 2 + 1;
            ULONG bp = PnSbcLadderBitpool(maxbp, minbp, fit, level);
            CHECK(bp <= prev && bp >= minbp);
            prev = bp;
        }
        ULONG fit = (maxbp + minbp) / 2 + 1, seen = 0, oldSeen = 0;
        for (ULONG level = 0; level <= 25; ++level) {
            seen += PnSbcLadderBitpool(maxbp, minbp, fit, level) == fit;
            oldSeen += OldSbcLadder(maxbp, minbp, level) == fit;
        }
        if (fit > minbp && fit < maxbp) CHECK(seen >= 1);
        (void)oldSeen;
    }
}

/* P11: TPDF dither when rounding to 16 bits; exact where no rounding happens. */
static void TestDither()
{
    /* 0.3 LSB of DC (S16 input 1 at gain 0.3): plain rounding outputs 0 forever */
    const ULONG n = 48000;
    std::vector<SHORT> one(n, 1), out(n);
    PN_PCM_STATE st = {};
    st.Dither = TRUE;
    CHECK(PnConvertPcmEx(&st, (UCHAR*)one.data(), n * 2, 1, 16, 16, FALSE, (UCHAR*)out.data(), n * 2, PNA2DP_FMT_S16, 1, 9830) == n * 2);
    double mean = 0; int worst = 0;
    for (SHORT v : out) { mean += v; worst = PN_MAX(worst, std::abs((int)v)); }
    mean /= n;
    CHECK(mean > 0.28 && mean < 0.32);   /* unbiased: 9830 / 32768 = 0.29999 */
    CHECK(worst <= 1);                   /* +-1 LSB triangular: within 1.5 LSB of the value */
    /* deterministic from the stream-start state */
    std::vector<SHORT> again(n);
    PN_PCM_STATE st2 = {}; st2.Dither = TRUE;
    PnConvertPcmEx(&st2, (UCHAR*)one.data(), n * 2, 1, 16, 16, FALSE, (UCHAR*)again.data(), n * 2, PNA2DP_FMT_S16, 1, 9830);
    CHECK(again == out);

    /* a 24-bit tone of 0.4 LSB16: undithered it vanishes (error = -signal); dithered the
     * output follows it and the error is uncorrelated with it */
    const double pi = 3.14159265358979323846;
    std::vector<LONG> tone(n);
    for (ULONG i = 0; i < n; ++i) tone[i] = (LONG)std::lround(0.4 * 65536.0 * std::sin(2 * pi * 1000.0 * i / 48000.0)) & ~0xff;
    PN_PCM_STATE st3 = {}; st3.Dither = TRUE;
    PnConvertPcmEx(&st3, (UCHAR*)tone.data(), n * 4, 1, 32, 24, FALSE, (UCHAR*)out.data(), n * 2, PNA2DP_FMT_S16, 1, 32768);
    double sx = 0, sxx = 0, sxe = 0, sxy = 0;
    for (ULONG i = 0; i < n; ++i) {
        double x = tone[i] / 65536.0, e = out[i] - x;
        sx += x; sxx += x * x; sxe += x * e; sxy += x * out[i];
    }
    CHECK(std::fabs(sxe / sxx) < 0.1);           /* error uncorrelated with the signal */
    CHECK(sxy / sxx > 0.9 && sxy / sxx < 1.1);    /* the tone survives at its level */

    /* no dither where nothing is rounded: 16-bit input at unity, silence, other formats */
    ULONG random = 0xC0FFEE;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    std::vector<ULONG> src(256);
    for (ULONG &v : src) v = next();
    for (ULONG fmt : {(ULONG)PNA2DP_FMT_S16, (ULONG)PNA2DP_FMT_S24, (ULONG)PNA2DP_FMT_S32}) for (ULONG outCh : {1u, 2u}) {
        std::vector<UCHAR> a(2048), b(2048);
        PN_PCM_STATE g = {}; g.Dither = TRUE;
        ULONG na = PnConvertPcm((UCHAR*)src.data(), 1020, 2, 16, 16, FALSE, a.data(), 2048, fmt, outCh, 32768);
        ULONG nb = PnConvertPcmEx(&g, (UCHAR*)src.data(), 1020, 2, 16, 16, FALSE, b.data(), 2048, fmt, outCh, 32768);
        CHECK(na == nb && na && memcmp(a.data(), b.data(), na) == 0);
    }
    PN_PCM_STATE mute = {}; mute.Dither = TRUE; mute.GainValid = TRUE; mute.GainQ15 = 0;
    PnConvertPcmEx(&mute, (UCHAR*)src.data(), 1024, 2, 32, 24, FALSE, (UCHAR*)out.data(), 1024, PNA2DP_FMT_S16, 2, 0);
    bool silent = true;
    for (ULONG i = 0; i < 256; ++i) silent = silent && out[i] == 0;
    CHECK(silent);
}

/* P11: the kernel SBC chain now converts to S32 and encodes with sbc_encode_frame_s32.
 * For 16-bit input at unity gain that is bit-exact with the previous S16 chain. */
static void TestSbcS32Chain()
{
    sbc_params p;
    p.sample_rate = 44100; p.channel_mode = SBC_MODE_JOINT; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = 53;
    sbc_encoder a, b;
    CHECK(sbc_encoder_init(&a, &p) == 0 && sbc_encoder_init(&b, &p) == 0);
    const double pi = 3.14159265358979323846;
    std::vector<SHORT> pcm(128 * 2 * 40);
    for (size_t i = 0; i < pcm.size() / 2; ++i) {
        pcm[i * 2] = (SHORT)std::lround(30000.0 * std::sin(2 * pi * 440.0 * i / 44100.0));
        pcm[i * 2 + 1] = (SHORT)std::lround(20000.0 * std::sin(2 * pi * 5100.0 * i / 44100.0));
    }
    std::vector<SHORT> s16(pcm.size()); std::vector<LONG> s32(pcm.size());
    PN_PCM_STATE sa = {}, sb = {};
    PnConvertPcmEx(&sa, (UCHAR*)pcm.data(), (ULONG)pcm.size() * 2, 2, 16, 16, FALSE, (UCHAR*)s16.data(), (ULONG)s16.size() * 2, PNA2DP_FMT_S16, 2, 32768);
    PnConvertPcmEx(&sb, (UCHAR*)pcm.data(), (ULONG)pcm.size() * 2, 2, 16, 16, FALSE, (UCHAR*)s32.data(), (ULONG)s32.size() * 4, PNA2DP_FMT_S32, 2, 32768);
    bool same = true;
    for (int f = 0; f < 40; ++f) {
        UCHAR fa[512], fb[512];
        ULONG la = sbc_encode_frame(&a, (const sbc_s16*)s16.data() + f * 256, fa, sizeof(fa));
        ULONG lb = sbc_encode_frame_s32(&b, (const sbc_s32*)s32.data() + f * 256, fb, sizeof(fb));
        same = same && la == 119 && la == lb && memcmp(fa, fb, la) == 0;
    }
    CHECK(same);
}

/* frequency response of a 2:1 FIR table (double, test only) */
static double Dec2GainDb(const PN_DEC2_TABLE *d, double f /* cycles per input sample */)
{
    const double pi = 3.14159265358979323846;
    double h = d->Coef[0] / 268435456.0;
    for (ULONG k = 1; k <= d->Center; ++k) h += 2.0 * d->Coef[k] / 268435456.0 * std::cos(2 * pi * f * k);
    return 20.0 * std::log10(PN_MAX(std::fabs(h), 1e-15));
}

/* amplitude of a tone of `freq` Hz in x[skip..] at `rate` (least squares) */
static double ToneAmplitude(const std::vector<LONG> &x, size_t skip, double freq, double rate)
{
    const double pi = 3.14159265358979323846;
    double c = 0, s = 0; size_t n = 0;
    for (size_t i = skip; i < x.size(); ++i, ++n) { c += x[i] * std::cos(2 * pi * freq * i / rate); s += x[i] * std::sin(2 * pi * freq * i / rate); }
    return 2.0 * std::sqrt(c * c + s * s) / (double)n;
}

static std::vector<LONG> Decimate(const std::vector<LONG> &in, ULONG outFmtBytes = 4)
{
    PN_PCM_STATE st = {}; st.Decimate = 2;
    std::vector<LONG> out(in.size() / 2 + 1);
    ULONG bytes = PnConvertPcmEx(&st, (const UCHAR*)in.data(), (ULONG)in.size() * 4, 1, 32, 32, FALSE, (UCHAR*)out.data(),
                                 (ULONG)out.size() * outFmtBytes, PNA2DP_FMT_S32, 1, 32768);
    out.resize(bytes / 4);
    return out;
}

/* P12/C8: the 2:1 decimator for 96 -> 48 kHz and 88.2 -> 44.1 kHz (0.1.0.22: alias-free FIRs, the
 * stopband from fs_out / 2; a state zeroed with Decimate 2 takes g_PnDec88, valid at both rates). */
static void TestDecimator2to1()
{
    const double pi = 3.14159265358979323846;
    /* the tables: DC gain exactly 1, ripple to 20 kHz, stopband from fs_out / 2 */
    for (const PN_DEC2_TABLE *d : { &g_PnDec96, &g_PnDec88 }) {
        const double fin = d == &g_PnDec96 ? 96000.0 : 88200.0;
        LONGLONG sum = d->Coef[0];
        for (ULONG k = 1; k <= d->Center; ++k) sum += 2ll * d->Coef[k];
        CHECK(sum == (1ll << 28) && d->Taps == 2 * d->Center + 1);
        double ripple = 0, stop = -1000;
        for (int i = 0; i <= 2000; ++i) ripple = PN_MAX(ripple, std::fabs(Dec2GainDb(d, 20000.0 / fin * i / 2000.0)));   /* to 20 kHz */
        for (int i = 0; i <= 2000; ++i) stop = PN_MAX(stop, Dec2GainDb(d, 0.25 + 0.25 * i / 2000.0));                    /* from fs_out / 2 */
        printf("2:1 decimator %.1f kHz: passband ripple %.6f dB to 20 kHz, stopband %.1f dB from fs_out / 2\n", fin / 1000.0, ripple, stop);
        CHECK(ripple < 0.01);
        CHECK(stop <= -110.0);
    }

    /* output frames: 2 in -> 1 out; an odd frame waits for its partner across blocks */
    {
        PN_PCM_STATE st = {}; st.Decimate = 2;
        LONG in[8] = {}, out[8];
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in, 8 * 4, 1, 32, 32, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 4 * 4);
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in, 3 * 4, 1, 32, 32, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 1 * 4 && st.DecPhase == 1);
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in, 1 * 4, 1, 32, 32, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 1 * 4 && st.DecPhase == 0);
        /* an output buffer smaller than the input consumes exactly the frames it writes */
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in, 8 * 4, 1, 32, 32, FALSE, (UCHAR*)out, 3 * 4, PNA2DP_FMT_S32, 1, 32768) == 3 * 4 && st.DecPhase == 0);
    }

    /* DC: exact after the filter has filled */
    {
        std::vector<SHORT> dc(4096 * 2, 16000), out(2048 * 2);
        PN_PCM_STATE st = {}; st.Decimate = 2;
        CHECK(PnConvertPcmEx(&st, (UCHAR*)dc.data(), 4096 * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), 2048 * 4, PNA2DP_FMT_S16, 2, 32768) == 2048 * 4);
        bool exact = true;
        for (ULONG f = PN_DEC2_88_TAPS; f < 2048; ++f) exact = exact && out[f * 2] == 16000 && out[f * 2 + 1] == 16000;
        CHECK(exact);
    }

    for (double rate : {96000.0, 88200.0}) {
        const size_t n = 96000;
        /* passband: 1 kHz and 20 kHz keep their level */
        for (double freq : {1000.0, 20000.0}) {
            std::vector<LONG> x(n);
            for (size_t i = 0; i < n; ++i) x[i] = (LONG)std::lround(1.0e9 * std::sin(2 * pi * freq * i / rate));
            double a = ToneAmplitude(Decimate(x), 200, freq, rate / 2);
            double db = 20.0 * std::log10(a / 1.0e9);
            CHECK(std::fabs(db) < 0.01);
        }
        /* stopband: every tone from 0.55 fs_out to fs_in/2 is at least 60 dB down */
        double worst = -1000;
        for (double fr = 0.55; fr < 1.0; fr += 0.05) {
            double freq = fr * rate / 2;
            std::vector<LONG> x(n);
            for (size_t i = 0; i < n; ++i) x[i] = (LONG)std::lround(1.0e9 * std::sin(2 * pi * freq * i / rate));
            std::vector<LONG> y = Decimate(x);
            double alias = rate - freq;   /* where it would fold to in the output */
            if (alias > rate / 2) alias -= rate / 2;
            double rms = 0;
            for (size_t i = 200; i < y.size(); ++i) rms += (double)y[i] * y[i];
            rms = std::sqrt(rms / (y.size() - 200)) * std::sqrt(2.0);
            worst = PN_MAX(worst, 20.0 * std::log10(PN_MAX(rms, 1.0) / 1.0e9));
        }
        CHECK(worst <= -60.0);
    }

    /* block boundaries: any block split (odd sizes, stereo, S16 out, gain) gives the same output */
    {
        const ULONG frames = 9000;
        std::vector<LONG> x(frames * 2);
        ULONG random = 0x5EED;
        auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
        for (ULONG i = 0; i < frames; ++i) {
            x[i * 2] = (LONG)std::lround(1.2e9 * std::sin(2 * pi * 3000.0 * i / 96000.0)) + (LONG)(next() >> 8) - (1 << 23);
            x[i * 2 + 1] = (LONG)(next() >> 2) - (1 << 29);
        }
        std::vector<SHORT> whole(frames), split(frames);
        PN_PCM_STATE a = {}; a.Decimate = 2;
        ULONG na = PnConvertPcmEx(&a, (UCHAR*)x.data(), frames * 8, 2, 32, 32, FALSE, (UCHAR*)whole.data(), frames * 2, PNA2DP_FMT_S16, 2, 23198);
        PN_PCM_STATE b = {}; b.Decimate = 2; b.GainValid = TRUE; b.GainQ15 = 23198;
        ULONG done = 0, nb = 0;
        while (done < frames) {
            ULONG chunk = PN_MIN(frames - done, 1 + next() % 701);
            nb += PnConvertPcmEx(&b, (UCHAR*)&x[done * 2], chunk * 8, 2, 32, 32, FALSE, (UCHAR*)split.data() + nb, frames * 2 - nb, PNA2DP_FMT_S16, 2, 23198);
            done += chunk;
        }
        CHECK(na == frames / 2 * 4 && nb == na && memcmp(whole.data(), split.data(), na) == 0);
        /* a zeroed state is a fresh filter: the next stream does not hear the previous one */
        std::vector<SHORT> first(frames), fresh(frames);
        memset(&a, 0, sizeof(a)); a.Decimate = 2;   /* PumpStartStream zeroes the state */
        PnConvertPcmEx(&a, (UCHAR*)&x[4000 * 2], 2000 * 8, 2, 32, 32, FALSE, (UCHAR*)first.data(), frames * 2, PNA2DP_FMT_S16, 2, 32768);
        PN_PCM_STATE c = {}; c.Decimate = 2;
        PnConvertPcmEx(&c, (UCHAR*)&x[4000 * 2], 2000 * 8, 2, 32, 32, FALSE, (UCHAR*)fresh.data(), frames * 2, PNA2DP_FMT_S16, 2, 32768);
        CHECK(first == fresh);
    }

    /* a trim crossfade counts engine-rate frames while decimating */
    {
        std::vector<SHORT> fadeSrc(192 * 2, 1000), in(256 * 2, -1000), out(128 * 2);
        PN_PCM_STATE st = {}; st.Decimate = 2; st.FadeOut = (const UCHAR*)fadeSrc.data(); st.FadeFrames = 192;
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in.data(), 128 * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), 128 * 4, PNA2DP_FMT_S16, 2, 32768) == 64 * 4);
        CHECK(st.FadePos == 128 && st.FadeFrames == 192);
    }
}

/* Review #6: a transient link stall is not throughput. The write held across the stall
 * and the backlog behind it used to make a ~280 ms stall look like a 60 kbps link, and the
 * urgent trim jumped SBC to the floor rung (about 100 s back up). Writes whose own send
 * time exceeds the stall bound stay out of the budget; one urgent rung follows. */
static void TestAbrStallNotThroughput()
{
    const LONGLONG ms = 10000;
    UCHAR offer[] = { 0x14, 0x15, 2, 53 };   /* SBC 48 kHz dual channel, 16 blocks, 8 subbands, bitpool 53 */
    A2DP_CODEC_CONFIG cfg = {};
    CHECK(A2dpParseConfig(A2DP_CODEC_SBC, offer, sizeof(offer), &cfg));
    cfg.SbcBitpool = cfg.SbcMaxBitpool;
    ULONG ladder[25], maxLevel = 24;
    for (ULONG i = 0; i <= 24; ++i) ladder[i] = A2dpQualityAtLevel(&cfg, i, 128000, NULL);
    while (maxLevel && ladder[maxLevel] == ladder[maxLevel - 1]) maxLevel--;
    CHECK(maxLevel >= 10);
    const ULONG payload = 672, packetUs = 8000;     /* 3 frames of 224 bytes: 8 ms of media */
    const ULONG bound = PnTxStallBoundUs(packetUs);
    CHECK(bound >= 4 * packetUs);
    /* a sustained slow link (each write takes twice its media time) is still measured */
    CHECK(bound > 2 * packetUs);

    /* worst case of the review: the stalled write and the 3 queued behind it in the window */
    PN_TX_LEDGER l = {};
    ULONG send = 0;
    l.LastDoneHns = 0;
    CHECK(PnTxLedgerPush(&l, 384, 1 * ms) && PnTxLedgerPush(&l, 384, 5 * ms) && PnTxLedgerPush(&l, 384, 9 * ms) && PnTxLedgerPush(&l, 384, 13 * ms));
    PnTxLedgerCompleteEx(&l, 1 * ms, 281 * ms, payload, bound, &send);
    CHECK(send == 280000 && l.Stalls == 1 && l.BusyUs == 0 && l.DeliveredBits == 0);
    PnTxLedgerCompleteEx(&l, 5 * ms, 282 * ms, payload, bound, &send);
    PnTxLedgerCompleteEx(&l, 9 * ms, 283 * ms, payload, bound, &send);
    PnTxLedgerCompleteEx(&l, 13 * ms, 284 * ms, payload, bound, &send);
    CHECK(l.DeliveredFrames == 4 * 384 && l.Stalls == 1 && l.BusyUs == 3000);
    PN_ABR_SAMPLE s = {};
    s.ElapsedMs = 250; s.Completed = 4; s.Stalls = l.Stalls; s.QueueFull = 30; s.Trims = 1;
    s.BacklogPeakUs = 260000; s.TargetLatencyUs = 60000;
    s.DeliveredMediaUs = 32000; s.DeliveredBits = l.DeliveredBits; s.BusyUs = l.BusyUs;
    CHECK(PnAbrBudgetBps(&s) == 0);                 /* three measured writes: not measurable */
    PN_QUALITY_WINDOW w = {};
    ULONG reasons = 0;
    RED_P6(PnAbrDecide(&w, &s, ladder, TRUE, 0, maxLevel, &reasons) == 1);

    /* the same stall with the backlog drained at the healthy 3.75 ms per write */
    PN_TX_LEDGER m = {};
    PnTxLedgerPush(&m, 384, 0);
    PnTxLedgerCompleteEx(&m, 0, 300 * ms, payload, bound, &send);
    LONGLONG t = 300 * ms;
    for (ULONG i = 1; i < 33; ++i) {
        PnTxLedgerPush(&m, 384, (LONGLONG)i * ms);
        t += 37500;
        PnTxLedgerCompleteEx(&m, (LONGLONG)i * ms, t, payload, bound, &send);
    }
    s.Completed = 33; s.Stalls = m.Stalls; s.DeliveredBits = m.DeliveredBits; s.BusyUs = m.BusyUs;
    CHECK(m.Stalls == 1 && m.BusyUs == 32 * 3750);
    CHECK(PnAbrBudgetBps(&s) > ladder[0]);          /* the link sustains the top rung */
    w = {};
    RED_P6(PnAbrDecide(&w, &s, ladder, TRUE, 0, maxLevel, &reasons) == 1);

    /* without a bound (0) every write counts, as PnTxLedgerComplete does */
    PN_TX_LEDGER n = {};
    PnTxLedgerPush(&n, 384, 0);
    PnTxLedgerCompleteEx(&n, 0, 281 * ms, payload, 0, &send);
    CHECK(n.Stalls == 0 && n.BusyUs == 281000 && n.DeliveredBits == payload * 8);

    /* Review round 3 #4: a total stall under 2 s (the C4 declaration) trims every ~190 ms,
     * so every 250 ms window is urgent (trim, queue full, nothing completed). One urgent
     * rung per episode, not one per window: the Sony path (bitpool 53) used to lose 4 rungs
     * in a 1 s stall and 7 in 1.75 s. */
    {
        PN_QUALITY_WINDOW q = {};
        PN_ABR_SAMPLE ok = {};
        ok.ElapsedMs = 250; ok.Completed = 31; ok.TargetLatencyUs = 60000;
        ok.DeliveredMediaUs = 250000; ok.DeliveredBits = 31ull * payload * 8; ok.BusyUs = 31 * 3750;
        ULONG level = 0;
        for (int i = 0; i < 40; ++i) {   /* 10 s healthy */
            int c = PnAbrDecide(&q, &ok, ladder, TRUE, level, maxLevel, &reasons);
            CHECK(c == 0);
        }
        PN_ABR_SAMPLE stall = {};
        stall.ElapsedMs = 250; stall.QueueFull = 60; stall.Trims = 1; stall.BacklogPeakUs = 250000; stall.TargetLatencyUs = 60000;
        for (int i = 0; i < 7; ++i) {    /* 1.75 s */
            int c = PnAbrDecide(&q, &stall, ladder, TRUE, level, maxLevel, &reasons);
            if (c > 0) level += (ULONG)c;
        }
        RED_R2("R3-4", level == 1);
        /* the stall ends: a clean window re-arms it, the next episode gets its urgent rung */
        int c = PnAbrDecide(&q, &ok, ladder, TRUE, level, maxLevel, &reasons);
        CHECK(c == 0 && !q.UrgentSpent);
        c = PnAbrDecide(&q, &stall, ladder, TRUE, level, maxLevel, &reasons);
        RED_R2("R3-4", c == 1);
        /* a congested window that delivered but found the queue full does not re-arm */
        PN_ABR_SAMPLE busy = ok; busy.QueueFull = 3; busy.BacklogPeakUs = 80000;
        PnAbrDecide(&q, &busy, ladder, TRUE, level + 1, maxLevel, &reasons);
        CHECK(q.UrgentSpent);
    }
}

/* Review #8: digital silence stays digital silence: no TPDF dither on an all-zero block,
 * whatever made the block qualify for dither (gain, a wider or float source, decimation). */
static void TestDitherDigitalSilence()
{
    const ULONG n = 1024;
    std::vector<UCHAR> zero(n * 2 * 4, 0);
    std::vector<SHORT> out(n * 2);
    auto allZero = [&](ULONG count) { for (ULONG i = 0; i < count; ++i) if (out[i]) return false; return true; };
    PN_PCM_STATE a = {}; a.Dither = TRUE;
    for (int block = 0; block < 4; ++block) {
        CHECK(PnConvertPcmEx(&a, zero.data(), n * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), n * 4, PNA2DP_FMT_S16, 2, 10362) == n * 4);
        RED_P8(allZero(n * 2));                     /* -10 dB software volume */
    }
    PN_PCM_STATE b = {}; b.Dither = TRUE;
    PnConvertPcmEx(&b, zero.data(), n * 8, 2, 32, 24, FALSE, (UCHAR*)out.data(), n * 4, PNA2DP_FMT_S16, 2, 32768);
    RED_P8(allZero(n * 2));                         /* 24-in-32 at unity */
    PN_PCM_STATE c = {}; c.Dither = TRUE;
    PnConvertPcmEx(&c, zero.data(), n * 8, 2, 32, 32, TRUE, (UCHAR*)out.data(), n * 4, PNA2DP_FMT_S16, 2, 32768);
    RED_P8(allZero(n * 2));                         /* float at unity */
    PN_PCM_STATE d = {}; d.Dither = TRUE; d.Decimate = 2;
    ULONG bytes = PnConvertPcmEx(&d, zero.data(), n * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), n * 4, PNA2DP_FMT_S16, 2, 32768);
    RED_P8(bytes == n * 2 && allZero(n));          /* 2:1 decimation */
    /* a block that is not silent is still dithered (0.3 LSB DC survives on average) */
    std::vector<SHORT> one(n * 2, 1);
    PN_PCM_STATE e = {}; e.Dither = TRUE;
    PnConvertPcmEx(&e, (UCHAR*)one.data(), n * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), n * 4, PNA2DP_FMT_S16, 2, 9830);
    CHECK(!allZero(n * 2));
}

/* Review round 2, #4: an AAC ceiling below 256 kbps (the MTU fit) does not repeat as
 * rung 1, so the first ABR step down is a real reduction. #5: the baseband-packet SDU
 * cap applies only where it costs fewer slots per frame. */
static void TestReview2P()
{
    {
        A2DP_CODEC_CONFIG cfg = {};
        cfg.Codec = PNA2DP_CODEC_AAC; cfg.SampleRate = 48000; cfg.Channels = 2;
        cfg.MaxBitrateBps = cfg.BitrateBps = PnAacMaxBitrateForPayload(672 - 12, 48000);
        CHECK(cfg.MaxBitrateBps == 243000);
        ULONG l0 = A2dpQualityAtLevel(&cfg, 0, 128000, NULL), l1 = A2dpQualityAtLevel(&cfg, 1, 128000, NULL);
        RED_R2("R2-4", l0 == 243000 && l1 == 192000);
        CHECK(A2dpQualityAtLevel(&cfg, 2, 128000, NULL) == 128000 && A2dpQualityAtLevel(&cfg, 3, 128000, NULL) == 128000);
        CHECK(A2dpQualityAtLevel(&cfg, 3, 0, NULL) == 96000 && A2dpQualityAtLevel(&cfg, 9, 0, NULL) == 96000);
        /* every rung strictly below the previous one until the floor (no repeated top) */
        for (ULONG mtu = 300; mtu <= 1100; mtu += 9) for (ULONG rate : {44100u, 48000u}) {
            cfg.SampleRate = rate;
            cfg.MaxBitrateBps = cfg.BitrateBps = PnAacMaxBitrateForPayload(mtu - 12, rate);
            if (!cfg.MaxBitrateBps) continue;
            ULONG prev = A2dpQualityAtLevel(&cfg, 0, 0, NULL);
            CHECK(prev == cfg.MaxBitrateBps);
            for (ULONG level = 1; level <= 4; ++level) {
                ULONG b = A2dpQualityAtLevel(&cfg, level, 0, NULL);
                CHECK(b < prev || b == 96000 || b == cfg.MaxBitrateBps);
                if (b == prev && b != 96000) RED_R2("R2-4", b == cfg.MaxBitrateBps && cfg.MaxBitrateBps <= 96000);
                prev = b;
            }
        }
        /* a large ceiling keeps the previous ladder exactly */
        cfg.SampleRate = 44100; cfg.MaxBitrateBps = cfg.BitrateBps = 320000;
        CHECK(A2dpQualityAtLevel(&cfg, 1, 0, NULL) == 256000 && A2dpQualityAtLevel(&cfg, 2, 0, NULL) == 192000 &&
              A2dpQualityAtLevel(&cfg, 3, 0, NULL) == 128000 && A2dpQualityAtLevel(&cfg, 4, 0, NULL) == 96000);
        cfg.MaxBitrateBps = cfg.BitrateBps = 256000;   /* a receiver ceiling at a rung */
        RED_R2("R2-4", A2dpQualityAtLevel(&cfg, 1, 0, NULL) == 192000);
    }
    {
        const ULONG hdr = 13;
        /* slot model: 2-DH5 (6 slots with the return slot) + a 2-DH1 tail (2) */
        CHECK(PnSduSlots(675, 675) == 6 && PnSduSlots(675, 676) == 8 && PnSduSlots(675, 1100) == 12);
        CHECK(PnSduSlots(335, 818) == 16);   /* DH5 + DH5 + DH3 */
        CHECK(PnSduSlots(363, 237) == 4 && PnSduSlots(1017, 1013) == 6 && PnSduSlots(23, 23) == 2);
        CHECK(PnSduSlots(600, 100) == 0);    /* not a baseband packet size */
        /* BR-only adapter, the conservative profile: joint 51 at 48 kHz = 115-byte frames,
         * MTU 895, 60 ms. 7 frames (DH5 + DH5 + DH3: 16 slots) beat 2 per DH5 (6 slots) */
        CHECK(SbcLen(48000, SBC_MODE_JOINT, 51) == 115);
        const ULONG us48 = 128u * 1000000u / 48000u;
        RED_R2("R2-5", PnSbcPacking(115, us48, 60, 895 - hdr, hdr, 335) == 7);
        /* 2 Mbps EDR, 3-slot only, 176-byte frames at MTU 672: 3 frames (2 x 2-DH3: 8 slots)
         * beat 1 per 2-DH3 (4 slots each) */
        RED_R2("R2-5", PnSbcPacking(176, us48, 60, 672 - hdr, hdr, 363) == 3);
        /* where one packet is cheaper the cap still applies (2-DH5: 5 x 119 in one packet) */
        CHECK(PnSbcPacking(119, 128u * 1000000u / 44100u, 60, 895 - hdr, hdr, 675) == 5);
        /* never more slots per frame than the previous rule, on any radio */
        for (ULONG sdu : {23u, 179u, 335u, 363u, 548u, 675u, 1017u}) for (ULONG fb = 20; fb <= 520; fb += 3)
            for (ULONG room : {659u, 882u, 992u}) {
                ULONG n = PnSbcPacking(fb, us48, 60, room, hdr, sdu), old = PnSbcPacking(fb, us48, 60, room, hdr, 0);
                CHECK(PnSduSlots(sdu, hdr + n * fb) * old <= PnSduSlots(sdu, hdr + old * fb) * n);
            }
    }
}

/* Review round 4. #3: a write's own send time includes the stack's and the controller's
 * fixed latency; the Sony path (48 kHz dual, bitpool 53, 3 frames = 8 ms per 685-byte write)
 * measured 2-8 ms completions, so "slower than its media time" alone walked SBC down on
 * ordinary jitter. #4: VBR AAC writes carry 1024 frames whatever their size; a 27-byte
 * access unit of digital silence is not 0.8 ms of media. #5: SBC XQ sizes its bitpool to the
 * EDR packet both ends support. */
static void TestReview4P()
{
    const ULONG sonyUs = PnTxWriteMediaUs(384, 48000, 685, 672000);
    CHECK(sonyUs == 8000);
    RED_R2("R4-3", !PnTxWriteSlow(9000, sonyUs) && !PnTxWriteSlow(17000, sonyUs));
    CHECK(PnTxWriteSlow(30000, sonyUs));                     /* beyond HEAD's 26 ms as well */
    const ULONG aacUs = PnTxWriteMediaUs(1024, 44100, 27, 256000);
    CHECK(aacUs == 23219);
    RED_R2("R4-4", !PnTxWriteSlow(5000, aacUs) && PnTxStallBoundUs(aacUs) == 4 * aacUs);
    CHECK(PnTxWriteSlow(40000, aacUs));
    /* a fragment without frames (or an unknown rate): estimated from its size, as before */
    CHECK(PnTxWriteMediaUs(0, 44100, 883, 256000) == 27593 && PnTxWriteMediaUs(1024, 0, 883, 256000) == 27593);
    CHECK(PnTxWriteMediaUs(0, 44100, 100, 0) == 20000);
    {
        PN_TX_LEDGER l = {};
        CHECK(PnTxLedgerOldestFrames(&l) == 0);
        PnTxLedgerPush(&l, 0, 0); PnTxLedgerPush(&l, 1024, 1);
        CHECK(PnTxLedgerOldestFrames(&l) == 0);
        ULONG send;
        PnTxLedgerCompleteEx(&l, 0, 30000, 100, 0, &send);
        CHECK(PnTxLedgerOldestFrames(&l) == 1024);
    }
    /* a window of silent VBR AAC at 5 ms completions: not congestion, and with the TX
     * queue never full no budget jump either (BusyUs then measures latency, not capacity) */
    {
        PN_ABR_SAMPLE s = {};
        s.ElapsedMs = 250; s.Completed = 11; s.TargetLatencyUs = 60000;
        s.DeliveredMediaUs = 11ull * aacUs; s.DeliveredBits = 11 * 15 * 8; s.BusyUs = 11 * 5000;
        for (ULONG i = 0; i < s.Completed; ++i) if (PnTxWriteSlow(5000, aacUs)) s.Slow++;
        RED_R2("R4-4", s.Slow == 0 && PnAbrReasons(&s) == 0);
        /* failures in such a window: a reduction, but one rung (the budget is meaningless) */
        s.Failed = 2;
        ULONG ladder[5] = {256000, 192000, 128000, 96000, 96000}, reasons = 0;
        PN_QUALITY_WINDOW w = {};
        int change = 0;
        for (unsigned i = 0; i < 8 && change == 0; ++i) change = PnAbrDecide(&w, &s, ladder, TRUE, 0, 3, &reasons);
        RED_R2("R4-4", change == 1);
        /* with the queue full the budget still jumps (P10) */
        s.QueueFull = 3;
        w = {}; change = 0;
        for (unsigned i = 0; i < 8 && change == 0; ++i) change = PnAbrDecide(&w, &s, ladder, TRUE, 0, 3, &reasons);
        CHECK(change == 3);
    }
    /* #5: local 3-DH5 adapter; the sink's features decide 47 (3-DH5) or 38 (2-DH5) */
    {
        const ULONGLONG edr = 3ull | (1ull << 25) | (1ull << 26) | (1ull << 39) | (1ull << 40);
        RED_R2("R4-5", PnSbcXqSduBytes(TRUE, edr, FALSE, 0) == PN_SDU_2DH5);              /* sink unknown */
        RED_R2("R4-5", PnSbcXqSduBytes(TRUE, edr, TRUE, edr & ~(1ull << 26)) == PN_SDU_2DH5); /* 2 Mbps sink */
        CHECK(PnSbcXqSduBytes(TRUE, edr, TRUE, edr) == PN_SDU_3DH5);
        CHECK(PnSbcXqSduBytes(TRUE, edr & ~(1ull << 26), TRUE, edr) == PN_SDU_2DH5);      /* 2 Mbps adapter */
        CHECK(PnSbcXqSduBytes(FALSE, 0, TRUE, edr) == 0);                                  /* adapter unknown: as before */
        /* 38 packs 4 frames into one 2-DH5 at MTU 895 */
        CHECK(PnSbcPacking(164, 128u * 1000000u / 48000u, 60, 895 - 13, 13, PnSbcXqSduBytes(TRUE, edr, FALSE, 0)) == 4);
    }
}

/* ---- Sound-quality review (known issue SBC XQ and findings #1-#34) ----------------
 * RED_R2("Q<n>", ...) marks a check that failed before the fix for finding n. */

static A2DP_CODEC_CONFIG SelectSbc(UCHAR Byte0, UCHAR MaxBp, ULONG Flags, ULONG Rate)
{
    UCHAR ie[] = { Byte0, 0xff, 2, MaxBp };
    A2DP_CODEC_CAPS caps; A2DP_CODEC_CONFIG cfg = {};
    PNA2DP_CONFIG pref = {};
    pref.Flags = Flags;
    if (!A2dpParseCaps(A2DP_CODEC_SBC, ie, sizeof(ie), &caps, NULL, NULL) || !A2dpSelectConfig(&caps, &pref, Rate, 16, &cfg))
        RtlZeroMemory(&cfg, sizeof(cfg));
    return cfg;
}

static ULONG PlanBitrate(const A2DP_CODEC_CONFIG &Cfg, const A2DP_SBC_STREAM_PLAN &Plan)
{
    A2DP_CODEC_CONFIG c = Cfg;
    c.SbcBitpool = Plan.StartBitpool;
    return A2dpNominalBitrate(&c);
}

/* SBC XQ never streams below plain dual channel for the same receiver, MTU and radio; it
 * helps where the default would be joint stereo (a low bitpool limit), and only its packing
 * follows the EDR packet both ends take. */
static void TestSbcXqRule()
{
    const ULONG hdr = 13;
    /* Sony CMT-SBT40D: caps ff ff 02 35 (max 53), MTU 895, 48 kHz */
    A2DP_CODEC_CONFIG xq = SelectSbc(0xff, 53, PNA2DP_CFG_SBC_XQ, 48000);
    A2DP_CODEC_CONFIG dual = SelectSbc(0xff, 53, PNA2DP_CFG_DUAL_CHANNEL_PREFER, 48000);
    CHECK(xq.ChannelMode == PNA2DP_CHMODE_DUAL && dual.ChannelMode == PNA2DP_CHMODE_DUAL);
    RED_R2("XQ", xq.SbcMaxBitpool == 53);
    const ULONG sdu3 = PN_SDU_3DH5, sdu2 = PN_SDU_2DH5;
    struct { ULONG local, xqSdu; } radios[] = { { 0, 0 }, { sdu3, sdu3 }, { sdu3, sdu2 }, { sdu2, sdu2 } };
    for (auto r : radios) {
        A2DP_SBC_STREAM_PLAN px, pd;
        CHECK(A2dpSbcPlanStream(&xq, TRUE, r.local, r.xqSdu, 895 - hdr, hdr, 60, &px) && px.Xq);
        CHECK(A2dpSbcPlanStream(&dual, FALSE, r.local, r.xqSdu, 895 - hdr, hdr, 60, &pd) && !pd.Xq);
        /* bitpool 53 = 672 kbps, never 47 (600 kbps) or 38 (492 kbps) */
        RED_R2("XQ", px.StartBitpool == 53 && px.CeilingBitpool == 53 && PlanBitrate(xq, px) == 672000);
        CHECK(PlanBitrate(xq, px) == PlanBitrate(dual, pd));
        CHECK(px.FramesPerPacket * 224 <= 895 - hdr && px.FramesPerPacket == 3);
        CHECK(px.SduBytes == (r.xqSdu ? r.xqSdu : sdu2));
    }
    /* 44.1 kHz: 617 kbps */
    xq = SelectSbc(0xff, 53, PNA2DP_CFG_SBC_XQ, 44100);
    {
        A2DP_SBC_STREAM_PLAN px;
        CHECK(A2dpSbcPlanStream(&xq, TRUE, sdu3, sdu3, 895 - hdr, hdr, 60, &px));
        RED_R2("XQ", px.StartBitpool == 53 && PlanBitrate(xq, px) == 617400);
    }
    /* MTU 1017 with 3-DH5 at both ends: 4 frames of 224 bytes (909) in one 3-DH5 at 672 kbps */
    xq = SelectSbc(0xff, 53, PNA2DP_CFG_SBC_XQ, 48000);
    {
        A2DP_SBC_STREAM_PLAN px;
        CHECK(A2dpSbcPlanStream(&xq, TRUE, sdu3, sdu3, 1017 - hdr, hdr, 60, &px));
        RED_R2("XQ", px.StartBitpool == 53 && PlanBitrate(xq, px) == 672000);
        CHECK(px.FramesPerPacket == 4 && hdr + 4 * 224 <= sdu3 && PnSduSlots(sdu3, hdr + 4 * 224) == 6);
    }
    /* a sink whose bitpool limit is 35 (the joint-stereo budget) but which takes dual channel:
     * XQ streams dual 35 (456 kbps) where the default is joint 35 (249 kbps) */
    {
        A2DP_CODEC_CONFIG def = SelectSbc(0xff, 35, 0, 48000), x = SelectSbc(0xff, 35, PNA2DP_CFG_SBC_XQ, 48000);
        A2DP_SBC_STREAM_PLAN pdef, px;
        CHECK(def.ChannelMode == PNA2DP_CHMODE_JOINT && x.ChannelMode == PNA2DP_CHMODE_DUAL);
        CHECK(A2dpSbcPlanStream(&def, FALSE, sdu3, sdu3, 895 - hdr, hdr, 60, &pdef));
        CHECK(A2dpSbcPlanStream(&x, TRUE, sdu3, sdu3, 895 - hdr, hdr, 60, &px));
        CHECK(PlanBitrate(def, pdef) == 249000 && PlanBitrate(x, px) == 456000 && px.StartBitpool == 35);
    }
    /* everywhere: XQ's bitpool is plain dual channel's, its packets fit the MTU, its bitpool
     * never exceeds the sink's */
    unsigned cases = 0, same = 0;
    for (UCHAR maxbp : { (UCHAR)12, (UCHAR)20, (UCHAR)35, (UCHAR)47, (UCHAR)53, (UCHAR)64, (UCHAR)80, (UCHAR)128, (UCHAR)250 })
    for (ULONG rate : { 44100u, 48000u }) for (ULONG mtu : { 120u, 300u, 672u, 679u, 895u, 1005u, 1017u, 1021u, 2048u })
    for (auto r : radios) {
        A2DP_CODEC_CONFIG x = SelectSbc(0xff, maxbp, PNA2DP_CFG_SBC_XQ, rate), d = SelectSbc(0xff, maxbp, PNA2DP_CFG_DUAL_CHANNEL_PREFER, rate);
        A2DP_SBC_STREAM_PLAN px, pd;
        BOOLEAN okx = A2dpSbcPlanStream(&x, TRUE, r.local, r.xqSdu, mtu - hdr, hdr, 60, &px);
        BOOLEAN okd = A2dpSbcPlanStream(&d, FALSE, r.local, r.xqSdu, mtu - hdr, hdr, 60, &pd);
        ++cases;
        if (okx != okd) continue;
        if (!okx) { ++same; continue; }
        ULONG len = SbcLen(rate, SBC_MODE_DUAL, px.StartBitpool);
        if (px.StartBitpool == pd.StartBitpool && PlanBitrate(x, px) == PlanBitrate(d, pd) && px.StartBitpool <= maxbp &&
            px.FramesPerPacket >= 1 && px.FramesPerPacket * len <= mtu - hdr) ++same;
    }
    CHECK(same == cases);
}

/* Findings #6/#8: SBC packs every bitpool it streams at - an ABR rung, a live change or the
 * user's lowered ceiling after a restart - so no packet overflows the MTU, and a lower rung
 * frees baseband air. */
static ULONG SonySlotsPerSecond(const A2DP_CODEC_CONFIG &c, ULONG frames, ULONG sdu)
{
    ULONG len = SbcLen(c.SampleRate, SBC_MODE_DUAL, c.SbcBitpool);
    ULONG framesPerSec = c.SampleRate / 128;
    return (ULONG)((ULONGLONG)PnSduSlots(sdu, 13 + frames * len) * framesPerSec / frames);
}

static void TestSbcRepack()
{
    const ULONG hdr = 13;
    A2DP_CODEC_CONFIG neg = SelectSbc(0xff, 53, PNA2DP_CFG_DUAL_CHANNEL_PREFER, 48000);
    CHECK(neg.SbcMaxBitpool == 53);
    /* the user lowered the bitpool live to 30, then the stream restarted (pause/resume) */
    A2DP_CODEC_CONFIG active = neg;
    active.SbcBitpool = 30;
    A2DP_SBC_STREAM_PLAN plan;
    CHECK(A2dpSbcPlanStream(&active, FALSE, PN_SDU_3DH5, PN_SDU_3DH5, 895 - hdr, hdr, 60, &plan));
    CHECK(plan.StartBitpool == 30 && plan.CeilingBitpool == 53 && plan.FramesPerPacket == 6);
    /* the ABR's top rung stays the user's 30: the first "reduction" no longer raises it to 46 */
    A2DP_CODEC_CONFIG s = active, lvl;
    s.SbcBitpool = s.SbcMaxBitpool = plan.StartBitpool;
    s.SbcFitBitpool = plan.FitBitpool;
    A2dpQualityAtLevel(&s, 1, 128000, &lvl);
    RED_R2("Q6", lvl.SbcBitpool < 30);
    /* a live raise back to 53 (up to the ceiling) repacks: 3 frames of 224 bytes, not 6 (1344) */
    s.SbcMaxBitpool = 53;
    A2dpQualityAtLevel(&s, 0, 128000, &lvl);
    ULONG n = A2dpSbcFramesPerPacket(&lvl, 60, 895 - hdr, hdr, plan.SduBytes);
    RED_R2("Q6", lvl.SbcBitpool == 53 && n == 3 && n * 224 <= 895 - hdr);

    /* invariant: every start bitpool, every rung, every MTU and radio: packets fit the MTU */
    unsigned bad = 0, total = 0;
    for (ULONG mtu : { 120u, 300u, 672u, 895u, 1005u, 2048u }) for (ULONG sdu : { 0u, 363u, 548u, 675u, 1017u })
    for (ULONG mode : { (ULONG)PNA2DP_CFG_DUAL_CHANNEL_PREFER, (ULONG)0 }) for (ULONG start = 2; start <= 53; start += 3) {
        A2DP_CODEC_CONFIG a = SelectSbc(0xff, 53, mode, 48000);
        a.SbcBitpool = (UCHAR)start;
        A2DP_SBC_STREAM_PLAN p;
        if (!A2dpSbcPlanStream(&a, FALSE, sdu, sdu, mtu - hdr, hdr, 60, &p)) continue;
        A2DP_CODEC_CONFIG b = a;
        b.SbcBitpool = b.SbcMaxBitpool = p.StartBitpool;
        b.SbcFitBitpool = p.FitBitpool;
        for (ULONG top : { (ULONG)p.StartBitpool, (ULONG)p.CeilingBitpool }) {
            b.SbcMaxBitpool = (UCHAR)top;   /* the user ceiling, or a live raise to the negotiated one */
            for (ULONG level = 0; level <= 24; ++level) {
                A2dpQualityAtLevel(&b, level, 128000, &lvl);
                ULONG frames = A2dpSbcFramesPerPacket(&lvl, 60, mtu - hdr, hdr, p.SduBytes);
                ULONG len = SbcLen(48000, lvl.ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL : SBC_MODE_JOINT, lvl.SbcBitpool);
                ++total;
                if (!frames || frames > 15 || frames * len > mtu - hdr) ++bad;
            }
        }
    }
    CHECK(total > 1000 && bad == 0);

    /* #8: the Sony ladder (dual 53, MTU 895, 3-DH5): each rung costs no more air than the one
     * above, and the reductions free air all the way down (fixed packing: 500 slots/s from
     * bitpool 40 to the floor) */
    {
        A2DP_CODEC_CONFIG b = neg;
        A2DP_SBC_STREAM_PLAN p;
        CHECK(A2dpSbcPlanStream(&b, FALSE, PN_SDU_3DH5, PN_SDU_3DH5, 895 - hdr, hdr, 60, &p));
        CHECK(p.StartBitpool == 53 && p.FramesPerPacket == 3 && p.FitBitpool == 52);   /* the validated Sony packing */
        b.SbcBitpool = b.SbcMaxBitpool = p.StartBitpool; b.SbcFitBitpool = p.FitBitpool;
        ULONG prev = 0xffffffffu, at40 = 0, at26 = 0, at53 = 0;
        BOOLEAN monotonic = TRUE;
        for (ULONG level = 0; level <= 24; ++level) {
            A2dpQualityAtLevel(&b, level, 128000, &lvl);
            ULONG frames = A2dpSbcFramesPerPacket(&lvl, 60, 895 - hdr, hdr, p.SduBytes);
            ULONG slots = SonySlotsPerSecond(lvl, frames, PN_SDU_3DH5);
            monotonic = monotonic && slots <= prev;
            prev = slots;
            if (lvl.SbcBitpool == 53) at53 = slots;
            if (lvl.SbcBitpool == 40) at40 = slots;
            if (lvl.SbcBitpool == 26) at26 = slots;
            /* the 2-DH5 rung still fits one 2-DH5 packet */
            if (lvl.SbcBitpool == p.FitBitpool) CHECK(hdr + frames * SbcLen(48000, SBC_MODE_DUAL, lvl.SbcBitpool) <= PN_SDU_2DH5);
        }
        CHECK(monotonic && at53 == 750);
        RED_R2("Q8", at40 == 450 && at26 < at40 && at26 <= 321);
    }
}

/* Findings #16/#17/#33: aptX packets carry 4-byte (HD: 6-byte) codewords and fill the MTU in
 * multiples of 8, the parity sync period; aptX LL keeps short packets. */
static void TestAptxPacking()
{
    RED_R2("Q16", PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX, 895) == 160 && PnAptxCodewordBytes(PNA2DP_CODEC_APTX) == 4);
    RED_R2("Q16", PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX_HD, 883) == 144 && PnAptxCodewordBytes(PNA2DP_CODEC_APTX_HD) == 6);
    /* LL: 112 blocks (9.3 ms) on MTU 895 as before; on smaller MTUs longer than the old
     * half-filled packets (84 at 672), the deliberate trade of finding #10 */
    CHECK(PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX_LL, 895) == 112 && PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX_LL, 672) == 112);
    CHECK(PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX, 672) == 160 && PnAptxBlocksPerPacket(PNA2DP_CODEC_APTX_HD, 660) == 104);
    for (ULONG codec : { (ULONG)PNA2DP_CODEC_APTX, (ULONG)PNA2DP_CODEC_APTX_HD, (ULONG)PNA2DP_CODEC_APTX_LL })
        for (ULONG room = 8; room <= 2048; ++room) {
            ULONG n = PnAptxBlocksPerPacket(codec, room), cw = PnAptxCodewordBytes(codec);
            BOOLEAN ok = n >= 1 && (n * cw <= room || room < cw) && (n < PN_APTX_SYNC_BLOCKS || n % PN_APTX_SYNC_BLOCKS == 0) &&
                n <= (codec == PNA2DP_CODEC_APTX_LL ? PN_APTX_LL_MAX_BLOCKS : PN_APTX_MAX_BLOCKS);
            /* at least the old (half-size) packet's audio, rounded to the sync period */
            ULONG old = PN_MIN(room / (2 * cw), codec == PNA2DP_CODEC_APTX_HD ? 80u : 160u);
            if (codec != PNA2DP_CODEC_APTX_LL && old >= 8) ok = ok && n >= old - old % 8;
            if (!ok) { CHECK(ok); break; }
        }
    CHECK(PnLdacMediaMtuUsable(679) && !PnLdacMediaMtuUsable(678) && !PnLdacMediaMtuUsable(672));
}

/* Finding #19: one host stall writes off its whole window (4 blocks) and counted 4 failures:
 * a quarantine and a renegotiation to SBC. A stall episode is one failure (and one more per
 * 500 ms it lasts); a dead host is still quarantined. */
static void TestHostStallEpisodes()
{
    PN_HOST_HEALTH h = {};
    PN_HOST_STALL s = {};
    ULONG counted = 0, oldFailures = 0;
    BOOLEAN quarantined = FALSE;
    for (ULONG t : { 200u, 221u, 243u, 264u }) {   /* AAC: a 270 ms stall expires the window */
        ++oldFailures;
        if (PnHostStallCounts(&s, t)) { ++counted; quarantined = quarantined || PnHostHealthFailure(&h, t); }
    }
    CHECK(oldFailures == PN_HOST_FAILURE_LIMIT);   /* what used to quarantine */
    RED_R2("Q19", counted == 1 && !quarantined && !PnHostHealthQuarantined(&h, 300));
    PnHostStallEnd(&s); PnHostHealthSuccess(&h, 300);
    /* the next stall after a success is a new episode */
    CHECK(PnHostStallCounts(&s, 400) && !PnHostStallCounts(&s, 420));
    PnHostStallEnd(&s); PnHostHealthSuccess(&h, 450);
    /* a host that never answers again: a request written off every ~21 ms */
    ULONGLONG quarantineAt = 0;
    for (ULONGLONG t = 1000; t < 5000 && !quarantineAt; t += 21)
        if (PnHostStallCounts(&s, t) && PnHostHealthFailure(&h, t)) quarantineAt = t;
    CHECK(quarantineAt >= 2000 && quarantineAt <= 2600 && PnHostHealthQuarantined(&h, quarantineAt + 1));
}

/* Findings #2/#27: when Windows stops driving the receiver's absolute volume the receiver keeps
 * its level; the software gain only adds what the user asks beyond it. */
static void TestRemoteVolumeHandOff()
{
    const LONG v = -451453;   /* -6.89 dB, the Sony trace */
    CHECK(PnSoftwareVolumeDb16(TRUE, FALSE, 0, v) == 0);
    RED_R2("Q2", PnSoftwareVolumeDb16(FALSE, TRUE, v, v) == 0);          /* detach: not 2V */
    CHECK(PnSoftwareVolumeDb16(FALSE, TRUE, v, -20 * 65536) == -20 * 65536 - v);   /* the slider moves on from V */
    CHECK(PnSoftwareVolumeDb16(FALSE, TRUE, v, 0) == 0);                 /* 100 %: never above unity */
    CHECK(PnSoftwareVolumeDb16(FALSE, FALSE, v, v) == v);                /* no absolute volume ever: as before */
    CHECK(PnSoftwareVolumeDb16(FALSE, TRUE, 0, v) == v);
    CHECK(PnSoftwareVolumeDb16(FALSE, TRUE, -96 * 65536, -96 * 65536) == 0);
    CHECK(PnSoftwareVolumeDb16(FALSE, TRUE, -2, LONG_MIN) < 0);
}

/* Findings #3/#5/#25, harness Inf: PCM conversion keeps headroom until the gain, a stream that
 * starts mid-programme fades in, and fades are format-exact. */
static void TestPcmQuality()
{
    /* #3: +6 dBFS float (2.0) at -12 dB volume is a clean -6 dBFS, not clipped to -12 dBFS */
    {
        ULONG two = 0x40000000, minusTwo = 0xc0000000;
        ULONG in[2] = { two, minusTwo };
        LONG out[2] = {};
        ULONG g = PnVolumeGainQ15(-12 * 65536);
        CHECK(PnConvertPcm((UCHAR*)in, sizeof(in), 1, 32, 32, TRUE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, g) == sizeof(out));
        LONGLONG expect = (LONGLONG)(2ll << 31) * g / 32768;
        RED_R2("Q3", out[0] == (LONG)expect && out[1] == -(LONG)expect);
        /* at unity the store saturates once */
        CHECK(PnConvertPcm((UCHAR*)in, sizeof(in), 1, 32, 32, TRUE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == sizeof(out));
        CHECK(out[0] == 2147483647L && out[1] == (-2147483647L - 1));
    }
    /* #3: the decimator's overshoot of a full-scale square survives -6 dB volume */
    {
        const ULONG frames = 9600;
        std::vector<LONG> sq(frames), out(frames / 2);
        for (ULONG i = 0; i < frames; ++i) sq[i] = ((i / 16) & 1) ? (-2147483647L - 1) : 2147483647L;   /* 3 kHz at 96 kHz */
        PN_PCM_STATE st = {}; st.Decimate = 2; st.GainValid = TRUE; st.GainQ15 = PnVolumeGainQ15(-6 * 65536);
        PnConvertPcmEx(&st, (UCHAR*)sq.data(), frames * 4, 1, 32, 32, FALSE, (UCHAR*)out.data(), frames * 2, PNA2DP_FMT_S32, 1, st.GainQ15);
        LONG peak = 0;
        for (ULONG i = 200; i < frames / 2; ++i) peak = PN_MAX(peak, std::abs(out[i]));
        RED_R2("Q3", peak > (LONG)(0.53 * 2147483648.0));   /* clamped first: <= 0.5012 FS */
    }
    /* #5/#25: a stream that starts mid-programme ramps in over its first block */
    {
        const ULONG frames = 384;
        std::vector<SHORT> in(frames * 2, 16000), out(frames * 2);
        PN_PCM_STATE st;
        PnPcmStateReset(&st, 1, TRUE);
        CHECK(st.Decimate == 1 && st.GainValid && st.GainQ15 == 0);
        PnConvertPcmEx(&st, (UCHAR*)in.data(), frames * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), frames * 4, PNA2DP_FMT_S16, 2, 32768);
        RED_R2("Q25", out[0] < 100 && out[(frames - 1) * 2] == 16000);
        BOOLEAN rising = TRUE;
        for (ULONG f = 1; f < frames; ++f) rising = rising && out[f * 2] >= out[(f - 1) * 2] && out[f * 2] - out[(f - 1) * 2] <= 16000 / (int)frames + 1;
        CHECK(rising);
        /* the true onset of a stream (nothing was discarded) is untouched */
        PnPcmStateReset(&st, 1, FALSE);
        PnConvertPcmEx(&st, (UCHAR*)in.data(), frames * 4, 2, 16, 16, FALSE, (UCHAR*)out.data(), frames * 4, PNA2DP_FMT_S16, 2, 32768);
        CHECK(out[0] == 16000);
    }
    /* PnPcmRamp: a raised-cosine fade, every format, up and down */
    {
        SHORT s16[24];
        for (int i = 0; i < 24; ++i) s16[i] = 11000;
        PnPcmRamp((UCHAR*)s16, 12, 2, 16, 16, FALSE, 0, 10, TRUE);
        BOOLEAN shapeOk = TRUE;
        for (int f = 0; f < 10; ++f) {
            double x = (f + 1) / 11.0, want = 11000.0 * std::sin(1.5707963267948966 * x) * std::sin(1.5707963267948966 * x);
            shapeOk = shapeOk && std::fabs(s16[f * 2] - want) <= 2.0 && s16[f * 2] == s16[f * 2 + 1] && (f == 0 || s16[f * 2] > s16[f * 2 - 2]);
        }
        CHECK(shapeOk && s16[20] == 11000 && s16[23] == 11000);
        for (int i = 0; i < 24; ++i) s16[i] = -11000;
        PnPcmRamp((UCHAR*)s16, 12, 2, 16, 16, FALSE, 0, 10, FALSE);
        shapeOk = TRUE;
        for (int f = 0; f < 10; ++f) {
            double x = (10 - f) / 11.0, want = -11000.0 * std::sin(1.5707963267948966 * x) * std::sin(1.5707963267948966 * x);
            shapeOk = shapeOk && std::fabs(s16[f * 2] - want) <= 2.0;
        }
        CHECK(shapeOk && s16[20] == 0 && s16[23] == 0);
        /* the slope is continuous: the second difference of a fade of a constant stays small */
        std::vector<SHORT> c(400, 16000);
        PnPcmRamp((UCHAR*)c.data(), 400, 1, 16, 16, FALSE, 0, 96, TRUE);
        int worst2 = 0;
        for (int i = 1; i + 1 < 120; ++i) worst2 = PN_MAX(worst2, std::abs(c[i + 1] - 2 * c[i] + c[i - 1]));
        CHECK(worst2 <= 16000 * 10 / (97 * 97) + 2);   /* pi^2/2 / 97^2 of the level; a linear ramp's kink: 1/97 */
        /* a ramp continued across calls is the same ramp */
        SHORT a[16], b[16];
        for (int i = 0; i < 16; ++i) a[i] = b[i] = (SHORT)(1000 * i);
        PnPcmRamp((UCHAR*)a, 16, 1, 16, 16, FALSE, 0, 12, FALSE);
        PnPcmRamp((UCHAR*)b, 5, 1, 16, 16, FALSE, 0, 12, FALSE);
        PnPcmRamp((UCHAR*)(b + 5), 11, 1, 16, 16, FALSE, 5, 12, FALSE);
        CHECK(memcmp(a, b, sizeof(a)) == 0);
        UCHAR p24[6] = { 0x00, 0x00, 0x40, 0x00, 0x00, 0xc0 };   /* +0.5, -0.5 packed 24-bit */
        PnPcmRamp(p24, 1, 2, 24, 24, FALSE, 0, 1, TRUE);
        CHECK(p24[2] == 0x20 && p24[5] == 0xe0 && p24[0] == 0 && p24[3] == 0);
        LONG w32[2] = { 0x40000000, 0x7fffff00 };                /* 24-in-32: x = 1/3 -> 0.25; low byte stays zero */
        PnPcmRamp((UCHAR*)w32, 1, 2, 32, 24, FALSE, 0, 2, TRUE);
        CHECK(w32[0] == 0x10000000 && (w32[1] & 0xff) == 0 && w32[1] == 0x1fffff00);
        ULONG f32[2] = { 0x3f000000, 0xbf000000 };                /* +-0.5f -> +-0.25f */
        PnPcmRamp((UCHAR*)f32, 1, 2, 32, 32, TRUE, 0, 1, TRUE);
        CHECK(f32[0] == 0x3e800000 && f32[1] == 0xbe800000);
    }
}

/* Audio-quality review 2 (findings #3-#9). RED_R2("F<n>", ...) failed before the fix for
 * finding n. */
static float F32Of(ULONG Bits) { float f; memcpy(&f, &Bits, 4); return f; }

static void TestAudioQualityFixes()
{
    /* #5/#9: a fade keeps float over full scale: 2.0f at the first frame of a 96-frame fade-out
     * is about 2.0 * g, not 1.0 * g */
    {
        ULONG f[2] = { 0x40000000, 0xc0000000 };   /* +2.0f, -2.0f */
        PnPcmRamp((UCHAR*)f, 1, 2, 32, 32, TRUE, 0, 96, FALSE);
        RED_R2("F5", F32Of(f[0]) > 1.99f && F32Of(f[0]) < 2.0f && F32Of(f[1]) < -1.99f && F32Of(f[1]) > -2.0f);
        /* +6 dBFS DC at -12 dB volume: the first faded frame is no 6 dB step below the frame before */
        ULONG dc[8];
        for (ULONG &v : dc) v = 0x40000000;
        PnPcmRamp((UCHAR*)(dc + 4), 4, 1, 32, 32, TRUE, 0, 96, FALSE);
        LONG out[8] = {};
        CHECK(PnConvertPcm((UCHAR*)dc, sizeof(dc), 1, 32, 32, TRUE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, PnVolumeGainQ15(-12 * 65536)) == sizeof(out));
        RED_R2("F5", out[3] > 0 && out[3] - out[4] < out[3] / 100);
        /* the end of a fade-in meets the unfaded sample after it */
        ULONG up[2] = { 0x40000000, 0x40000000 };
        PnPcmRamp((UCHAR*)up, 2, 1, 32, 32, TRUE, 0, 1, TRUE);
        RED_R2("F5", F32Of(up[0]) == 1.0f && F32Of(up[1]) == 2.0f);   /* g = 0.5: was 0.5f */
        /* +4.0 and beyond stays at the +12 dBFS limit; in-range values are exact */
        ULONG big[2] = { 0x41000000, 0x3f000000 };   /* 8.0f, 0.5f */
        PnPcmRamp((UCHAR*)big, 2, 1, 32, 32, TRUE, 0, 1, TRUE);
        CHECK(F32Of(big[0]) == 2.0f && big[1] == 0x3f000000);
        /* integer formats are unchanged */
        SHORT s16[2] = { 32767, -32768 };
        PnPcmRamp((UCHAR*)s16, 1, 2, 16, 16, FALSE, 0, 1, TRUE);
        CHECK(s16[0] == 16383 && s16[1] == -16384);
    }
    /* #6: a NaN or Inf sample repeats the channel's last finite sample; a run fades to silence */
    {
        const ULONG half = 0x3f000000, nan = 0x7fc00000, pinf = 0x7f800000, ninf = 0xff800000;
        ULONG in[6] = { half, half, nan, half, ninf, half };
        LONG out[6] = {};
        PN_PCM_STATE st;
        PnPcmStateReset(&st, 0, FALSE);
        CHECK(PnConvertPcmEx(&st, (UCHAR*)in, sizeof(in), 1, 32, 32, TRUE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == sizeof(out));
        RED_R2("F6", out[2] == 0x40000000 && out[4] == 0x40000000);
        CHECK(out[0] == 0x40000000 && out[3] == 0x40000000 && out[5] == 0x40000000);
        /* a run of 40: held, then a linear fade to silence over PN_PCM_NONFINITE_HOLD samples */
        std::vector<ULONG> run(41, pinf);
        run[0] = half;
        std::vector<LONG> ro(41);
        PnPcmStateReset(&st, 0, FALSE);
        PnConvertPcmEx(&st, (UCHAR*)run.data(), 41 * 4, 1, 32, 32, TRUE, (UCHAR*)ro.data(), 41 * 4, PNA2DP_FMT_S32, 1, 32768);
        BOOLEAN fades = ro[1] == 0x40000000 && ro[PN_PCM_NONFINITE_HOLD + 1] == 0 && ro[40] == 0;
        for (ULONG i = 2; i <= PN_PCM_NONFINITE_HOLD + 1; ++i) fades = fades && ro[i] < ro[i - 1] && ro[i - 1] - ro[i] <= 0x40000000 / (LONG)PN_PCM_NONFINITE_HOLD + 1;
        RED_R2("F6", fades);
        /* per channel: NaN on R holds R; the stereo-to-mono mix uses the held R */
        ULONG st2[4] = { half, 0x3e800000, half, nan };   /* L 0.5, R 0.25, then L 0.5, R NaN */
        LONG s2[4] = {}, m1[2] = {};
        PnPcmStateReset(&st, 0, FALSE);
        PnConvertPcmEx(&st, (UCHAR*)st2, sizeof(st2), 2, 32, 32, TRUE, (UCHAR*)s2, sizeof(s2), PNA2DP_FMT_S32, 2, 32768);
        RED_R2("F6", s2[2] == 0x40000000 && s2[3] == 0x20000000);
        PnPcmStateReset(&st, 0, FALSE);
        PnConvertPcmEx(&st, (UCHAR*)st2, sizeof(st2), 2, 32, 32, TRUE, (UCHAR*)m1, sizeof(m1), PNA2DP_FMT_S32, 1, 32768);
        RED_R2("F6", m1[0] == 0x30000000 && m1[1] == 0x30000000);
        /* without a stream state (PnConvertPcm) a non-finite sample is silence, as before */
        LONG o1[6] = {};
        PnConvertPcm((UCHAR*)in, sizeof(in), 1, 32, 32, TRUE, (UCHAR*)o1, sizeof(o1), PNA2DP_FMT_S32, 1, 32768);
        CHECK(o1[2] == 0 && o1[4] == 0 && o1[3] == 0x40000000);
        /* a new stream forgets the last sample */
        PnPcmStateReset(&st, 0, FALSE);
        ULONG lone[1] = { nan };
        LONG lo[1] = { 1 };
        PnConvertPcmEx(&st, (UCHAR*)lone, 4, 1, 32, 32, TRUE, (UCHAR*)lo, 4, PNA2DP_FMT_S32, 1, 32768);
        CHECK(lo[0] == 0);
    }
    /* #3/#8: the receiver holds the level absolute volume last set only while the link that
     * carried it stays up */
    {
        const LONG v = -25 * 65536, user = -25 * 65536;
        BOOLEAN active = TRUE, holds = FALSE;
        LONG held = 0;
        /* an AVDTP session restart on a link that stays up (the Sony #2/#27 case): unity */
        PnRemoteVolumeDetached(&active, &holds, &held, v, TRUE);
        CHECK(!active && holds && PnSoftwareVolumeDb16(active, holds, held, user) == 0);
        /* the link drops: the receiver's level is unknown, software volume is the user's */
        PnRemoteVolumeLinkLost(&holds);
        RED_R2("F3", PnSoftwareVolumeDb16(active, holds, held, user) == user);
        /* the listener detaches only after the link went down: no hold either */
        active = TRUE; holds = FALSE; held = 0;
        PnRemoteVolumeDetached(&active, &holds, &held, v, FALSE);
        RED_R2("F3", !active && PnSoftwareVolumeDb16(active, holds, held, user) == user);
        RED_R2("F3", PnSoftwareVolumeDb16(active, holds, held, -6 * 65536) == -6 * 65536);   /* the slider is live */
    }
    /* #4: a stream start fades in only when the discarded PCM ended in a signal */
    {
        UCHAR ring[64] = {};
        /* rendered silence (the T2 resume path): no fade */
        RED_R2("F4", !PnRingTailHasSignal(ring, 64, 1000, 64, 16));
        /* the newest bytes carry a signal, also across the wrap */
        ring[(1000 - 1) % 64] = 1;
        CHECK(PnRingTailHasSignal(ring, 64, 1000, 64, 16));
        ring[(1000 - 1) % 64] = 0;
        ring[0] = 0x80;   /* 1000 % 64 = 40: bytes 24..39 are the newest 16; 0 is older */
        RED_R2("F4", !PnRingTailHasSignal(ring, 64, 1000, 64, 16));
        CHECK(PnRingTailHasSignal(ring, 64, 1000, 64, 48));       /* 48 bytes back reach byte 0 */
        CHECK(PnRingTailHasSignal(ring, 64, 70, 64, 8));          /* wraps: bytes 62, 63, 0..5 */
        CHECK(!PnRingTailHasSignal(ring, 64, 1000, 0, 16) && !PnRingTailHasSignal(NULL, 64, 1000, 64, 16));
        CHECK(!PnRingTailHasSignal(ring, 64, 8, 4, 64));          /* never more than is available: bytes 4..7 */
    }
}

/* 0.1.0.22 #1, requirement (f): every native-rate conversion stays bit-exact. One FNV-1a 64 over
 * the output of a fixed input set through every native combination: input formats (S16, S24,
 * S24-in-32, S32, F32 with over-range, NaN and Inf), targets (S16 with and without dither, S24,
 * S32, F32), stereo / mono both ways, unity, -6 dB, gain ramps and mute, a trim crossfade, a
 * fade-in start and digital silence, in uneven blocks. The constant was recorded from pcm.cpp
 * before the rate converter existed (the 0.1.0.21 conversion code). */
static ULONGLONG GoldenFnv(ULONGLONG h, const void *p, size_t n)
{
    const UCHAR *b = (const UCHAR *)p;
    for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    return h;
}

static void TestNativeConversionGolden()
{
    const ULONGLONG expected = 0x48aab6e64ad2c171ull;   /* pcm.cpp of 0.1.0.21 (bd164af) */
    ULONG random = 0x601DE11u;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    struct InFmt { ULONG Container, Valid; BOOLEAN Float; };
    const InFmt fmts[] = { {16, 16, FALSE}, {24, 24, FALSE}, {32, 24, FALSE}, {32, 32, FALSE}, {32, 32, TRUE} };
    const ULONG targets[] = { PNA2DP_FMT_S16, PNA2DP_FMT_S24, PNA2DP_FMT_S32, PNA2DP_FMT_F32 };
    const ULONG blocks[] = { 384, 1, 7, 128, 383, 1024, 2, 640 };
    /* gain per block: unity, -6 dB, a ramp down, mute, a ramp up, unity */
    const ULONG gains[] = { 32768, 16423, 16423, 9830, 0, 0, 23198, 32768 };
    ULONGLONG h = 0xCBF29CE484222325ull;
    ULONG combos = 0;
    for (const InFmt &f : fmts) for (ULONG target : targets) for (ULONG inCh = 1; inCh <= 2; ++inCh) for (ULONG outCh = 1; outCh <= 2; ++outCh)
    for (ULONG variant = 0; variant < 4; ++variant) {
        /* variant 0: zeroed state; 1: dither on; 2: fade-in start (PnPcmStateReset); 3: trim crossfade + dither */
        if (variant != 0 && variant != 2 && target != PNA2DP_FMT_S16) continue;
        ULONG bps = f.Container / 8, frameBytes = inCh * bps;
        std::vector<UCHAR> in(2048 * frameBytes), fade(192 * frameBytes), out(2048 * 2 * 4);
        auto fill = [&](std::vector<UCHAR> &buf, ULONG frames, BOOLEAN silent) {
            for (ULONG i = 0; i < frames * inCh; ++i) {
                UCHAR *s = buf.data() + i * bps;
                if (silent) { memset(s, 0, bps); continue; }
                if (f.Float) {
                    ULONG r = next(), bits;
                    float v = (float)((LONG)(r >> 1) - 0x40000000) / (float)0x30000000;   /* about +-1.33 */
                    memcpy(&bits, &v, 4);
                    if ((r & 0x3f) == 1) bits = 0x7fc00000;                     /* NaN */
                    else if ((r & 0x3f) == 2) bits = (r & 0x80) ? 0xff800000 : 0x7f800000;   /* -+Inf */
                    else if ((r & 0x3f) == 3) bits = 0x40a00000;                /* 5.0: over the +-4.0 limit */
                    else if ((r & 0x3f) == 4) bits = 0x00000001;                /* denormal */
                    else if ((r & 0x3f) == 5) bits = 0x80000000;                /* -0.0 */
                    memcpy(s, &bits, 4);
                } else {
                    ULONG r = next();
                    memcpy(s, &r, bps);
                }
            }
        };
        PN_PCM_STATE st;
        if (variant == 2) PnPcmStateReset(&st, 1, TRUE);
        else memset(&st, 0, sizeof(st));
        st.Dither = variant == 1 || variant == 3;
        for (ULONG b = 0; b < PN_ARRAYSIZE(blocks); ++b) {
            ULONG frames = blocks[b];
            fill(in, frames, b == 5);   /* block 5: digital silence */
            if (variant == 3 && b == 2) {
                fill(fade, 192, FALSE);
                st.FadeOut = fade.data(); st.FadeFrames = 192; st.FadePos = 0;
            }
            ULONG outBytes = PnConvertPcmEx(&st, in.data(), frames * frameBytes, inCh, f.Container, f.Valid, f.Float, out.data(), (ULONG)out.size(),
                                            target, outCh, gains[b]);
            h = GoldenFnv(h, &outBytes, sizeof(outBytes));
            h = GoldenFnv(h, out.data(), outBytes);
        }
        /* a capped output buffer converts the frames it has room for */
        ULONG capped = PnConvertPcmEx(&st, in.data(), 300 * frameBytes, inCh, f.Container, f.Valid, f.Float, out.data(), 100 * outCh * 4, target, outCh, 32768);
        h = GoldenFnv(h, &capped, sizeof(capped));
        h = GoldenFnv(h, out.data(), capped);
        combos++;
    }
    printf("native conversion golden: %u combinations, FNV-1a 64 %016llx\n", combos, (unsigned long long)h);
    CHECK(combos == 200);
    CHECK(h == expected);
}

static void TestQualityReviewP()
{
    TestSbcXqRule();
    TestSbcRepack();
    TestAptxPacking();
    TestHostStallEpisodes();
    TestRemoteVolumeHandOff();
    TestPcmQuality();
    TestAudioQualityFixes();
}

/* 0.1.0.22 #2: one source of defaults, validation and the save rule (config_policy.h). */
static void TestConfigPolicy()
{
    PNA2DP_CONFIG d;
    PnConfigDefaults(&d);
    CHECK(d.Size == sizeof(d) && d.Version == PNA2DP_ABI_VERSION);
    CHECK(d.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ && d.Ldac.AbrEnable == 1 && d.Ldac.FormatMask == 0);   /* was SQ */
    CHECK(d.Aac.BitrateBps == 320000 && d.Aac.Vbr == 0);                                              /* was 0 / VBR */
    CHECK(d.Sbc.MaxBitpool == 0 && d.Sbc.MinBitpool == 2 && !d.Sbc.ChannelModeMask);
    CHECK(!(d.Flags & (PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_SBC_XQ)));                         /* known issue #31 */
    CHECK(d.Flags == (PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_APPLY_LIVE | PNA2DP_CFG_SUSPEND_ON_SCO | PNA2DP_CFG_DELAY_REPORTING |
                      PNA2DP_CFG_SOFTWARE_VOLUME | PNA2DP_CFG_AUTO_QUALITY));
    CHECK(d.CodecPriority[0] == PNA2DP_CODEC_LDAC && d.CodecPriority[1] == PNA2DP_CODEC_APTX_HD && d.CodecPriority[2] == PNA2DP_CODEC_AAC &&
          d.CodecPriority[3] == PNA2DP_CODEC_APTX && d.CodecPriority[4] == PNA2DP_CODEC_APTX_LL && d.CodecPriority[5] == PNA2DP_CODEC_SBC && !d.CodecPriority[6]);
    CHECK(d.TargetLatencyMs == 60 && d.MaxLatencyMs == 250 && d.AbrFloorBps == 128000 && d.ReconnectDelayMs == 2000 && d.VolumeDb16 == 0);
    PNA2DP_CONFIG v = d;
    CHECK(PnConfigValidate(&v) && !memcmp(&v, &d, sizeof(v)));   /* the driver stores the defaults as they are */
    /* the validation is the driver's (unchanged clamps, max 500 = ring 600 - headroom 100) */
    v = d; v.TargetLatencyMs = 5; v.MaxLatencyMs = 1000;
    CHECK(PnConfigValidate(&v) && v.TargetLatencyMs == 20 && v.MaxLatencyMs == 500);
    static_assert(PN_CONFIG_MAX_LATENCY_MS == 600 - PN_RING_HEADROOM_MS, "the latency ceiling stays below the ring");
    v = d; v.TargetLatencyMs = 450; v.MaxLatencyMs = 300;
    CHECK(PnConfigValidate(&v) && v.TargetLatencyMs == 400 && v.MaxLatencyMs == 420);
    v = d; v.Size = 4;
    CHECK(!PnConfigValidate(&v));
    v = d; v.Ldac.Eqmid = 9; v.Flags = PNA2DP_CFG_AUTO_QUALITY; v.Ldac.AbrEnable = 0; v.EnabledCodecMask = 0;
    CHECK(PnConfigValidate(&v) && v.Ldac.Eqmid == PNA2DP_LDAC_EQMID_SQ && (v.Flags & PNA2DP_CFG_ABR_ENABLE) && v.Ldac.AbrEnable &&
          (v.Flags & PNA2DP_CFG_SOFTWARE_VOLUME) && v.EnabledCodecMask == PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC));

    /* DeviceStart saves the defaults only where nothing is stored: never over a saved
     * configuration it cannot read (another ABI, a rejected value, an I/O error) */
    CHECK(PnConfigShouldSaveDefaults((LONG)0xC0000034L));    /* STATUS_OBJECT_NAME_NOT_FOUND */
    CHECK(PnConfigShouldSaveDefaults((LONG)0xC000003AL));    /* STATUS_OBJECT_PATH_NOT_FOUND */
    CHECK(!PnConfigShouldSaveDefaults((LONG)0xC0000004L));   /* STATUS_INFO_LENGTH_MISMATCH */
    CHECK(!PnConfigShouldSaveDefaults((LONG)0xC000000DL));   /* STATUS_INVALID_PARAMETER */
    CHECK(!PnConfigShouldSaveDefaults((LONG)0xC000009AL));   /* STATUS_INSUFFICIENT_RESOURCES */
    CHECK(!PnConfigShouldSaveDefaults((LONG)0x80000005L));   /* STATUS_BUFFER_OVERFLOW */
    CHECK(!PnConfigShouldSaveDefaults(0));
}

/* 0.1.0.22 #5: latency profiles are a target / max pair; all fit the ring unchanged. */
static void TestLatencyProfiles()
{
    const ULONG expect[4][2] = { {0, 0}, {30, 120}, {60, 250}, {150, 450} };
    for (ULONG p = PN_LATENCY_PROFILE_LOW; p <= PN_LATENCY_PROFILE_MUSIC; p++) {
        ULONG t = 0, m = 0;
        CHECK(PnLatencyProfileValues(p, &t, &m) && t == expect[p][0] && m == expect[p][1]);
        CHECK(PnLatencyProfileOf(t, m) == p);
        PNA2DP_CONFIG c;
        PnConfigDefaults(&c);
        PnApplyLatencyProfile(&c, p);
        CHECK(c.TargetLatencyMs == t && c.MaxLatencyMs == m);
        PNA2DP_CONFIG v = c;
        CHECK(PnConfigValidate(&v) && !memcmp(&v, &c, sizeof(v)));
        ULONG et, em;
        PnEffectiveLatencyMs(t, m, 600, &et, &em);
        CHECK(et == t && em == m);                    /* the pump applies them as configured */
    }
    ULONG t = 7, m = 8;
    CHECK(!PnLatencyProfileValues(PN_LATENCY_PROFILE_CUSTOM, &t, &m) && !PnLatencyProfileValues(9, &t, &m) && t == 7 && m == 8);
    CHECK(PnLatencyProfileOf(90, 333) == PN_LATENCY_PROFILE_CUSTOM);
    CHECK(PnLatencyProfileOf(150, 451) == PN_LATENCY_PROFILE_CUSTOM && PnLatencyProfileOf(80, 300) == PN_LATENCY_PROFILE_CUSTOM);
    PNA2DP_CONFIG c;
    PnConfigDefaults(&c);
    c.TargetLatencyMs = 90; c.MaxLatencyMs = 333;
    PnApplyLatencyProfile(&c, PN_LATENCY_PROFILE_CUSTOM);
    CHECK(c.TargetLatencyMs == 90 && c.MaxLatencyMs == 333);
    PnConfigDefaults(&c);
    CHECK(PnLatencyProfileOf(c.TargetLatencyMs, c.MaxLatencyMs) == PN_LATENCY_PROFILE_BALANCED);   /* the default */

    /* SBC packets: ~target/3 per packet up to 80 ms, so Music does not make 50 ms packets */
    CHECK(PnSbcPackingTargetMs(30) == 30 && PnSbcPackingTargetMs(60) == 60 && PnSbcPackingTargetMs(80) == 80 && PnSbcPackingTargetMs(150) == 80);
    CHECK(PnSbcPacking(119, 2667, PnSbcPackingTargetMs(150), 882, 13, 0) == PnSbcPacking(119, 2667, 80, 882, 13, 0));
}

/* 0.1.0.22 #5: the ABR rules scale with the buffer. Target <= 60 ms: the 0.1.0.21 rules
 * exactly (gate 0). A larger target lets the backlog absorb a hiccup: SLOW needs a backlog
 * over the gate, and a backlog the link is draining is no congestion. */
static void TestAbrProfileGate()
{
    CHECK(PnAbrSoftGateUs(60) == 0 && PnAbrSoftGateUs(30) == 0 && PnAbrSoftGateUs(20) == 0);
    CHECK(PnAbrSoftGateUs(150) == 90000 && PnAbrSoftGateUs(61) == 1000 && PnAbrSoftGateUs(400) == 340000);
    PN_ABR_SAMPLE s = AbrSample(3, 0, 80000, 18);
    s.TargetLatencyUs = 150000; s.SoftGateUs = PnAbrSoftGateUs(150); s.RingStartUs = 40000; s.RingEndUs = 80000;
    CHECK(PnAbrReasons(&s) == 0);                                   /* slow writes, backlog under the gate */
    s.BacklogPeakUs = 120000; s.RingEndUs = 120000;
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_SLOW);
    s.Slow = 0; s.DeliveredMediaUs = 200000;                        /* under real time, queue full */
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_SLOW);
    s.BacklogPeakUs = 80000; s.RingEndUs = 80000;
    CHECK(PnAbrReasons(&s) == 0);
    s = AbrSample(3, 0, 200000);
    s.TargetLatencyUs = 150000; s.SoftGateUs = 90000; s.RingStartUs = 150000; s.RingEndUs = 200000;
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_BACKLOG);       /* growing backlog over the target */
    s.RingStartUs = 300000; s.RingEndUs = 200000; s.BacklogPeakUs = 300000; s.DeliveredMediaUs = 400000;
    CHECK(PnAbrReasons(&s) == 0);                                   /* draining: the link has spare capacity */
    s.RingEndUs = 291000;
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_BACKLOG);       /* shrinking less than 10 ms is not draining */
    s.RingEndUs = 200000; s.Trims = 1;
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_BACKLOG);       /* a trim emptied the ring, not the link */
    s.Trims = 0; s.Failed = 1; s.Pressure = 1;
    CHECK(PnAbrReasons(&s) == (PNA2DP_QUALITY_REASON_FAILED | PNA2DP_QUALITY_REASON_PRESSURE));   /* losses always count */
    /* gate 0 (Balanced, Low latency): draining or not, the 0.1.0.21 rules */
    s = AbrSample(3, 0, 300000, 18);
    s.RingStartUs = 300000; s.RingEndUs = 100000;
    CHECK(PnAbrReasons(&s) == (PNA2DP_QUALITY_REASON_BACKLOG | PNA2DP_QUALITY_REASON_SLOW));
    s = AbrSample(0, 0, 0, 2);
    CHECK(PnAbrReasons(&s) == PNA2DP_QUALITY_REASON_SLOW);
}

/* A link carrying LDAC (990 / 660 / 330 kbps) in 10 ms steps, observed every 250 ms as the
 * pump does: the ring holds ~10 ms while the link keeps up; it grows during a stall or while
 * the link is slower than the stream; the latency guard trims to the target over the max. */
struct LinkSim {
    ULONG trims = 0, downgrades = 0, peakBacklogMs = 0, firstDowngradeMs = 0, finalBps = 0;
};
static LinkSim SimulateLink(ULONG TargetMs, ULONG MaxMs, bool ProfileRules, double CapacityX, ULONG StallMs, ULONG StallEveryMs,
                            double DegradedX, ULONG DegradeAtMs, ULONG DurationMs)
{
    const ULONG ladder[3] = { 990000, 660000, 330000 }, maxLevel = 2;
    const double base = 10;
    ULONG effTarget, effMax;
    PnEffectiveLatencyMs(TargetMs, MaxMs, 600, &effTarget, &effMax);
    LinkSim r;
    PN_QUALITY_WINDOW w = {};
    ULONG level = 0;
    double backlog = base, lastRing = base;
    PN_ABR_SAMPLE s = {};
    double delivered = 0, bits = 0, busy = 0;
    bool stalledBefore = false;
    for (ULONG t = 10; t <= DurationMs; t += 10) {
        double rate = ladder[level];
        double cap = (DegradeAtMs && t > DegradeAtMs ? DegradedX : CapacityX) * ladder[0];
        bool stall = StallMs && StallEveryMs && t > StallEveryMs / 2 && (t % StallEveryMs) > 0 && (t % StallEveryMs) <= StallMs;
        bool full;
        if (stall) {
            backlog += 10; full = true; stalledBefore = true;
        } else {
            double can = 10.0 * cap / rate, want = backlog + 10 - base;
            double sent = can < want ? can : want;
            full = can < want;
            backlog += 10 - sent;
            delivered += sent; bits += sent * rate / 1000.0;
            busy += full ? 10000.0 : sent * rate / cap * 1000.0;
            s.Completed += (ULONG)(sent / 5 + 0.5);      /* ~5 ms of LDAC per write */
            if (stalledBefore) { s.Slow++; s.Stalls++; s.Completed++; stalledBefore = false; }   /* the write the stall held */
        }
        if (full) { s.QueueFull++; if (backlog * 1000 > s.BacklogPeakUs) s.BacklogPeakUs = (ULONG)(backlog * 1000); }
        if (backlog > r.peakBacklogMs) r.peakBacklogMs = (ULONG)backlog;
        if (backlog > effMax) { backlog = effTarget; s.Trims++; r.trims++; }
        if (t % 250 == 0) {
            s.ElapsedMs = 250; s.TargetLatencyUs = effTarget * 1000;
            s.DeliveredMediaUs = (ULONGLONG)(delivered * 1000); s.DeliveredBits = (ULONGLONG)bits; s.BusyUs = (ULONGLONG)busy;
            if (ProfileRules) {
                s.SoftGateUs = PnAbrSoftGateUs(effTarget);
                s.RingStartUs = (ULONG)(lastRing * 1000); s.RingEndUs = (ULONG)(backlog * 1000);
            }
            lastRing = backlog;
            ULONG reasons = 0;
            int change = PnAbrDecide(&w, &s, ladder, TRUE, level, maxLevel, &reasons);
            if (change > 0) {
                level = PN_MIN(level + (ULONG)change, maxLevel); r.downgrades++;
                if (!r.firstDowngradeMs && t > DegradeAtMs) r.firstDowngradeMs = t - DegradeAtMs;
            } else if (change < 0) level--;
            s = {}; delivered = bits = busy = 0;
        }
    }
    r.finalBps = ladder[level];
    return r;
}

static void TestLatencyProfileLink()
{
    /* a link with 25% spare capacity and a 300 ms radio stall every 8 s, for 120 s */
    LinkSim bal = SimulateLink(60, 250, true, 1.25, 300, 8000, 0, 0, 120000);
    LinkSim music = SimulateLink(150, 450, true, 1.25, 300, 8000, 0, 0, 120000);
    LinkSim musicLegacy = SimulateLink(150, 450, false, 1.25, 300, 8000, 0, 0, 120000);
    CHECK(bal.trims >= 1 && bal.downgrades >= 1);              /* Balanced: trim at 250 ms, urgent downgrade */
    CHECK(music.trims == 0 && music.downgrades == 0);          /* Music rides it out at full quality */
    CHECK(music.peakBacklogMs <= 310);
    CHECK(musicLegacy.downgrades >= 1);                        /* the 0.1.0.21 rules downgrade even with the larger buffer */
    /* the 0.1.0.21 rules at the Balanced target behave exactly as the new code */
    LinkSim balLegacy = SimulateLink(60, 250, false, 1.25, 300, 8000, 0, 0, 120000);
    CHECK(balLegacy.trims == bal.trims && balLegacy.downgrades == bal.downgrades && balLegacy.finalBps == bal.finalBps);
    /* sustained congestion: the link drops to 0.8x the top rung after 10 s */
    LinkSim balSlow = SimulateLink(60, 250, true, 1.25, 0, 0, 0.8, 10000, 40000);
    LinkSim musicSlow = SimulateLink(150, 450, true, 1.25, 0, 0, 0.8, 10000, 40000);
    CHECK(balSlow.firstDowngradeMs && balSlow.firstDowngradeMs <= 1500);
    CHECK(musicSlow.firstDowngradeMs && musicSlow.firstDowngradeMs <= 2500 && musicSlow.trims == 0);
    CHECK(balSlow.finalBps <= 0.8 * 990000 && musicSlow.finalBps <= 0.8 * 990000);
    /* Low latency: the 0.1.0.21 rules at a 30 ms target */
    LinkSim low = SimulateLink(30, 120, true, 1.25, 300, 8000, 0, 0, 120000);
    CHECK(low.trims >= 1 && low.downgrades >= 1);
    printf("Latency profiles, 1.25x link with a 300 ms stall every 8 s for 120 s: Balanced %u trims, %u downgrades; "
           "Music %u trims, %u downgrades, peak backlog %u ms (0.1.0.21 rules: %u downgrades); Low latency %u trims. "
           "Link at 0.8x: Balanced downgrades after %u ms, Music after %u ms with %u trims\n",
           bal.trims, bal.downgrades, music.trims, music.downgrades, music.peakBacklogMs, musicLegacy.downgrades, low.trims,
           balSlow.firstDowngradeMs, musicSlow.firstDowngradeMs, musicSlow.trims);
}

static void TestLaneP3()
{
    TestReview2P();
    TestSbcPacking();
    TestDither();
    TestSbcS32Chain();
    TestDecimator2to1();
    TestAbrStallNotThroughput();
    TestDitherDigitalSilence();
    TestReview4P();
    TestQualityReviewP();
    TestConfigPolicy();
    TestLatencyProfiles();
    TestAbrProfileGate();
    TestLatencyProfileLink();
    TestNativeConversionGolden();
}

static void TestLaneP()
{
    TestHostHealthPolicy();
    TestHostLedger();
    TestHostResultBounds();
    TestAacMtuCap();
    TestTxLedger();
    TestPipelineLatency();
    TestLatencyGuard();
    TestGainRamp();
    TestTrimCrossfade();
    TestAbrSignals();
    TestAbrHalfThroughput();
    TestPumpWakeups();
    TestLaneP3();
}
