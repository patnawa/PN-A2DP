/* Integration (glue) tests: the cross-lane decisions wired in the integrated tree.
 *   G3  media stall: reported to the device once per episode (PnStallEdge)
 *   G6  jack: unplugged only by a user disconnect, plugged by any sign of the receiver;
 *       ACL loss and a stall keep the plugged default (review round 2 #7, round 5 #5)
 *   G7  user disconnect: no automatic connect until a new start / reconnect / incoming
 *   G9  codec recovery after a codec host quarantine: at the next start or pause, once
 *       per session, never mid-stream
 *   G14 F32 output of the PCM converter without floating point in the kernel
 * Included by core_test.cpp after core_test_T.inl (uses TWant / TPlanIs).
 * SPDX-License-Identifier: MIT */

static void TestGlueStallEdge()
{
    BOOLEAN reported = FALSE;
    CHECK(!PnStallEdge(FALSE, &reported) && !reported);
    CHECK(PnStallEdge(TRUE, &reported) && reported);      /* rising edge: report */
    for (int i = 0; i < 100; i++) CHECK(!PnStallEdge(TRUE, &reported));   /* still stalled: once */
    CHECK(!PnStallEdge(FALSE, &reported) && !reported);   /* writes complete again: re-arm */
    CHECK(PnStallEdge(TRUE, &reported));                  /* a new episode is reported */
    static_assert(PN_MEDIA_STALL_MS >= 1500 && PN_MEDIA_STALL_MS <= 2000, "a receiver, not the 200 ms host write-off");

    /* the ledger decides "stalled"; the edge sits on top of it */
    PN_TX_LEDGER l;
    memset(&l, 0, sizeof(l));
    CHECK(!PnTxLedgerStalled(&l, 1000000000ll, PN_MEDIA_STALL_MS));   /* nothing outstanding */
}

static void TestGlueJack()
{
    /* start-up behaviour is the adapter's: connected. Only a user disconnect unplugs; an
     * unanswered stall is the start of an outage the ACL going down ends (round 5 #5) */
    RED_R2("R5-5", PnJackAction(PnJackStallUnanswered, FALSE) == PN_JACK_KEEP);
    CHECK(PnJackAction(PnJackUserDisconnect, TRUE) == PN_JACK_UNPLUG);
    CHECK(PnJackAction(PnJackSignallingClosed, FALSE) == PN_JACK_KEEP);   /* idle close is no evidence */
    CHECK(PnJackAction(PnJackStallAnswered, FALSE) == PN_JACK_KEEP);      /* the receiver answered */
    /* any sign of the receiver brings an unplugged endpoint back */
    CHECK(PnJackAction(PnJackAclUp, FALSE) == PN_JACK_PLUG);
    CHECK(PnJackAction(PnJackSignallingUp, FALSE) == PN_JACK_PLUG);
    /* ONESHOT_RECONNECT plugs through the connect it causes, not before it */
    CHECK(PnJackAction(PnJackUserReconnect, FALSE) == PN_JACK_KEEP);
    /* review round 2 #7: the ACL going down (receiver switched off, standby, out of range)
     * does not unplug: nothing would reconnect a receiver that does not page us, while a
     * plugged endpoint stays the default and the next playback pages it. A user disconnect
     * holds. */
    RED_R2("R2-7", PnJackAction(PnJackAclDown, FALSE) == PN_JACK_PLUG);
    CHECK(PnJackAction(PnJackAclDown, TRUE) == PN_JACK_KEEP);
    {
        /* out of range while streaming: stall, unanswered probe, then the ACL supervision
         * timeout. The endpoint ends plugged so playback reconnects, and is never unplugged
         * on the way: Windows would move the audio to another device and back (round 5 #5) */
        BOOLEAN plugged = TRUE;
        unsigned flips = 0;
        ULONG a = PnJackAction(PnStallJackEvent(PN_ST_IO_TIMEOUT), FALSE);
        if (a != PN_JACK_KEEP) { flips += plugged != (a == PN_JACK_PLUG); plugged = a == PN_JACK_PLUG; }
        CHECK(!PnRetryUnreachable(FALSE, FALSE, TRUE, FALSE, FALSE));   /* no retry without the ACL */
        a = PnJackAction(PnJackAclDown, FALSE);
        if (a != PN_JACK_KEEP) { flips += plugged != (a == PN_JACK_PLUG); plugged = a == PN_JACK_PLUG; }
        RED_R2("R2-7", plugged);
        RED_R2("R5-5", flips == 0);
        /* a short supervision timeout: the ACL drops while the probe is out, which then
         * fails with the channel lost; nothing would plug the endpoint back (round 5 #5) */
        plugged = TRUE; flips = 0;
        a = PnJackAction(PnJackAclDown, FALSE);
        if (a != PN_JACK_KEEP) { flips += plugged != (a == PN_JACK_PLUG); plugged = a == PN_JACK_PLUG; }
        a = PnJackAction(PnStallJackEvent(PN_ST_CONNECTION_DISCONNECTED), FALSE);
        if (a != PN_JACK_KEEP) { flips += plugged != (a == PN_JACK_PLUG); plugged = a == PN_JACK_PLUG; }
        RED_R2("R5-5", plugged && flips == 0);
        /* idle when the receiver is switched off: stays plugged */
        plugged = TRUE;
        a = PnJackAction(PnJackAclDown, FALSE);
        if (a != PN_JACK_KEEP) plugged = a == PN_JACK_PLUG;
        RED_R2("R2-7", plugged);
        /* user disconnect, then the ACL drops as a result: stays unplugged */
        plugged = PnJackAction(PnJackUserDisconnect, TRUE) != PN_JACK_UNPLUG;
        a = PnJackAction(PnJackAclDown, TRUE);
        if (a != PN_JACK_KEEP) plugged = a == PN_JACK_PLUG;
        CHECK(!plugged);
    }

    CHECK(PnStallJackEvent(0) == PnJackStallAnswered);
    CHECK(PnStallJackEvent(PN_ST_DEVICE_PROTOCOL_ERROR) == PnJackStallAnswered);   /* a reject is an answer */
    CHECK(PnStallJackEvent(PN_ST_NOT_SUPPORTED) == PnJackStallAnswered);
    CHECK(PnStallJackEvent(PN_ST_IO_TIMEOUT) == PnJackStallUnanswered);
    CHECK(PnStallJackEvent(PN_ST_CONNECTION_DISCONNECTED) == PnJackStallUnanswered);
    CHECK(PnStallJackEvent(PN_ST_CANCELLED) == PnJackStallUnanswered);   /* not an answer; the step yields first */

    /* review round 5 #1: a probe or ABORT cancelled by the receiver's new signalling
     * session (or that session's reset pending) proves the receiver is there: the stall
     * step yields to the reset, it neither drops the new channel nor unplugs */
    RED_R2("R5-1", PnStallProbeAction(PN_ST_CANCELLED, TRUE, FALSE) == PnStallYield);
    RED_R2("R5-1", PnStallProbeAction(PN_ST_CANCELLED, FALSE, TRUE) == PnStallYield);
    RED_R2("R5-1", PnStallProbeAction(PN_ST_CONNECTION_DISCONNECTED, TRUE, TRUE) == PnStallYield);   /* old channel closed, new one accepted */
    RED_R2("R5-1", PnStallProbeAction(0, TRUE, TRUE) == PnStallYield);   /* an ABORT now would be cancelled too */
    CHECK(PnStallSessionReplaced(PN_ST_CANCELLED, FALSE) && PnStallSessionReplaced(PN_ST_IO_TIMEOUT, TRUE));
    CHECK(!PnStallSessionReplaced(0, FALSE) && !PnStallSessionReplaced(PN_ST_IO_TIMEOUT, FALSE) &&
          !PnStallSessionReplaced(PN_ST_CONNECTION_DISCONNECTED, FALSE));
    /* without a new session: as before */
    CHECK(PnStallProbeAction(PN_ST_IO_TIMEOUT, TRUE, FALSE) == PnStallDisconnect);
    CHECK(PnStallProbeAction(0, TRUE, FALSE) == PnStallAbort && PnStallProbeAction(0, FALSE, FALSE) == PnStallKeep);
}

static void TestGlueUserDisconnect()
{
    PN_RECONCILE_PLAN p;
    /* playing after a user disconnect: nothing connects and no retry is armed */
    PN_RECONCILE_INPUT in = TWant(AvdtpStateIdle);
    in.SignallingUp = FALSE;
    in.UserDisconnected = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(!PnPlanHas(&p, PnStepEnsureSignalling) && !PnPlanHas(&p, PnStepEnsureOpen) && p.RetryDelayMs == 0);
    CHECK(!PnReconcileWantsAudio(&in));
    /* the same input without the flag connects as before */
    in.UserDisconnected = FALSE;
    PnPlanReconcile(&in, &p);
    CHECK(PnPlanHas(&p, PnStepEnsureSignalling) && PnPlanHas(&p, PnStepStartPump));
    CHECK(PnReconcileWantsAudio(&in));
    /* a session the receiver opened meanwhile (the flag is cleared by the incoming
     * connection; if not yet) is never started by us while the flag stands */
    in = TWant(AvdtpStateOpen);
    in.UserDisconnected = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(!PnPlanHas(&p, PnStepStartIfOpen) && !PnPlanHas(&p, PnStepStartPump));
    /* a running pump is stopped (the worker has already dropped the session) */
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.UserDisconnected = TRUE; in.SuspendDue = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(PnPlanHas(&p, PnStepStopPump) && !PnPlanHas(&p, PnStepStartPump));
}

static void TestGlueCodecRecovery()
{
    PN_RECONCILE_PLAN p;
    /* our own SBC fallback stream; a host codec came back (snapshot: BetterCodecAvailable) */
    PN_RECONCILE_INPUT in = TWant(AvdtpStateStreaming);
    in.PumpStreaming = TRUE; in.BetterCodecAvailable = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(!PnPlanHas(&p, PnStepUpgradeCodec) && !PnPlanHas(&p, PnStepStopPump));   /* never mid-stream */
    /* at the next stream start (suspended stream, START pending) */
    in = TWant(AvdtpStateOpen); in.BetterCodecAvailable = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(PnPlanHas(&p, PnStepUpgradeCodec) && TPlanIndex(p, PnStepUpgradeCodec) < TPlanIndex(p, PnStepStartIfOpen));
    /* at the pause: once the stream is suspended */
    in = TWant(AvdtpStateStreaming); in.AudioRun = FALSE; in.PumpStreaming = TRUE; in.BetterCodecAvailable = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(!PnPlanHas(&p, PnStepUpgradeCodec));                   /* paused, not yet suspended */
    in.SuspendDue = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(PnPlanHas(&p, PnStepSuspend) && PnPlanHas(&p, PnStepUpgradeCodec) &&
          TPlanIndex(p, PnStepSuspend) < TPlanIndex(p, PnStepUpgradeCodec));
    /* ping-pong guard: once per signalling session */
    in = TWant(AvdtpStateOpen); in.BetterCodecAvailable = TRUE; in.CodecUpgradeTried = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(!PnPlanHas(&p, PnStepUpgradeCodec));
    /* the SBC-only receiver path: nothing better, the plan is unchanged */
    in = TWant(AvdtpStateOpen);
    PN_RECONCILE_PLAN plain;
    PnPlanReconcile(&in, &plain);
    CHECK(TPlanIs(plain, { PnStepEnsureSignalling, PnStepEnsureDiscovered, PnStepCheckFormat, PnStepEnsureOpen,
                           PnStepStartIfOpen, PnStepStartPump }));
}

static void TestGlueF32()
{
    /* S32 -> F32 through the converter (integer path in the kernel) against float math */
    static const LONG values[] = { 0, 1, -1, 2, 255, 256, 257, 0x7fffff, 0x800000, 0x800001, 0x1000001, 0x1000003,
                                   0x12345678, -0x12345678, 0x7fffff80, 0x7fffffc0, 0x7fffffff, (LONG)0x80000000, (LONG)0x80000001,
                                   -0x7fffffc0, 0x40000000, -0x40000000, 0x00ffffff, 0x01ffffff };
    unsigned bad = 0;
    auto one = [&](LONG v) {
        UCHAR in[4]; memcpy(in, &v, 4);
        UCHAR out[4] = {};
        ULONG n = PnConvertPcm(in, 4, 1, 32, 32, FALSE, out, sizeof(out), PNA2DP_FMT_F32, 1, 32768);
        float expect = (float)v / 2147483648.0f, got;
        memcpy(&got, out, 4);
        if (n != 4 || memcmp(&expect, &got, 4) != 0) { if (bad++ < 5) printf("F32 mismatch for %ld: got %.9g expected %.9g\n", (long)v, got, expect); }
    };
    for (LONG v : values) one(v);
    /* round-to-nearest-even: exact ties and their neighbours at every exponent that rounds */
    for (ULONG shift = 1; shift <= 7; shift++) {
        static const ULONG mant[] = { 0x800000, 0x800001, 0x800002, 0xfffffe, 0xffffff };
        for (ULONG m : mant) {
            LONGLONG tie = ((LONGLONG)m << shift) + (1ll << (shift - 1));
            for (LONGLONG d = -1; d <= 1; d++) {
                LONGLONG v = tie + d;
                if (v > 0x7fffffffll) continue;
                one((LONG)v);
                one((LONG)-v);
            }
        }
    }
    ULONG x = 0x12345678u;
    for (int i = 0; i < 200000; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; one((LONG)x); one((LONG)(x >> (i % 31))); }
    CHECK(bad == 0);
}

/* Review #11: after a stall whose probe went unanswered the session is dropped; with
 * playback paused or stopped meanwhile no audio pass retries, and while the ACL stays up
 * no ACL-up event will come either: the connect worker keeps trying signalling with the
 * reconnect back-off until the receiver answers, the ACL drops or the user disconnects. */
static void TestGlueUnreachableRetry()
{
    CHECK(PnRetryUnreachable(FALSE, FALSE, TRUE, TRUE, FALSE));
    CHECK(!PnRetryUnreachable(TRUE, FALSE, TRUE, TRUE, FALSE));    /* audio wanted: Reconcile retries */
    CHECK(!PnRetryUnreachable(FALSE, TRUE, TRUE, TRUE, FALSE));    /* user disconnect holds */
    CHECK(!PnRetryUnreachable(FALSE, FALSE, FALSE, TRUE, FALSE));  /* nothing went wrong */
    CHECK(!PnRetryUnreachable(FALSE, FALSE, TRUE, FALSE, FALSE));  /* ACL down: OnAcl brings it back */
    CHECK(!PnRetryUnreachable(FALSE, FALSE, TRUE, TRUE, TRUE));    /* signalling up again */
    /* the stall sequence: unanswered probe (the jack stays: round 5 #5), and the retries
     * keep going (capped) */
    CHECK(PnJackAction(PnStallJackEvent(PN_ST_IO_TIMEOUT), FALSE) == PN_JACK_KEEP);
    ULONG attempts = 0, totalMs = 0;
    for (int i = 0; i < 20; ++i) {
        ULONG d = PnReconnectDelayMs(attempts, 2000);
        CHECK(d <= 30000);
        totalMs += d;
        if (attempts < 10) attempts++;
    }
    CHECK(totalMs > 60000);   /* still trying after a minute; no attempt count ends it */
}

static void TestGlue()
{
    TestGlueStallEdge();
    TestGlueJack();
    TestGlueUserDisconnect();
    TestGlueCodecRecovery();
    TestGlueF32();
    TestGlueUnreachableRetry();
}
