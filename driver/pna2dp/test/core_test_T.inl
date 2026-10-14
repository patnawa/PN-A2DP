/* Lane T (transport) tests: characterization of the extracted pure decisions, and the
 * RED group that encodes the corrected transport behaviour (review items T1-T7, T9-T13
 * and scripted receivers). The RED group failed against the step-1 code (81 of 81) and
 * passes since the step-2 fixes; characterization checks marked "step 2" changed by design.
 * Step 3 (T8/T9 locking) added TestTLocking: its "T9-lock" RED checks (24) failed against
 * the step-2 decisions, where the acceptor could never run during our transaction.
 * Included by core_test.cpp; uses its CHECK / checks / failures.
 * SPDX-License-Identifier: MIT */
#include "transport_policy.h"
#include "sdp_policy.h"

static unsigned tRedChecks, tRedFailures, tNonRedFailures;
#define RED(id, c) do { ++checks; ++tRedChecks; if (!(c)) { ++failures; ++tRedFailures; printf("RED[%s] line %d: %s\n", id, __LINE__, #c); } } while (0)

/* ---- fixtures ---------------------------------------------------------------------- */

static const ULONG tLocalCodecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC };

/* Same table as device.cpp BuildLocalSeps: SEIDs 1..6. */
static void TBuildLocalSeps(AVDTP_LOCAL_SEP *Seps)
{
    memset(Seps, 0, sizeof(AVDTP_LOCAL_SEP) * 6);
    for (ULONG i = 0; i < 6; i++) {
        Seps[i].Seid = (UCHAR)(i + 1);
        Seps[i].MediaType = AVDTP_MEDIA_TYPE_AUDIO;
        Seps[i].Tsep = AVDTP_TSEP_SOURCE;
        Seps[i].Codec = tLocalCodecs[i];
        Seps[i].DelayReporting = TRUE;
        UCHAR mct;
        Seps[i].CodecCapsLength = (UCHAR)A2dpBuildLocalCaps(tLocalCodecs[i], Seps[i].CodecCaps, sizeof(Seps[i].CodecCaps), &mct);
    }
}

/* Service capabilities: media transport, [content protection...], codec, [delay reporting]. */
static ULONG TCapsBytes(UCHAR *Out, const UCHAR *CodecIes, ULONG IesLen, UCHAR CodecType, BOOLEAN Delay)
{
    ULONG n = 0;
    Out[n++] = AVDTP_CAT_MEDIA_TRANSPORT; Out[n++] = 0;
    n += AvdtpBuildMediaCodecCap(Out + n, 64, CodecType, CodecIes, IesLen);
    if (Delay) { Out[n++] = AVDTP_CAT_DELAY_REPORTING; Out[n++] = 0; }
    return n;
}

static AVDTP_REMOTE_SEP TSepFromCaps(UCHAR Seid, BOOLEAN InUse, const UCHAR *Caps, ULONG Len)
{
    AVDTP_REMOTE_SEP s;
    memset(&s, 0, sizeof(s));
    s.Seid = Seid; s.InUse = InUse; s.MediaType = AVDTP_MEDIA_TYPE_AUDIO; s.Tsep = AVDTP_TSEP_SINK;
    AvdtpParseServiceCapabilities(Caps, Len, &s);
    return s;
}

/* A sink SEP advertising our own local capability set for Codec. */
static AVDTP_REMOTE_SEP TSinkSep(UCHAR Seid, ULONG Codec, BOOLEAN InUse = FALSE, BOOLEAN Delay = FALSE)
{
    UCHAR ies[32], mct, caps[80];
    ULONG l = A2dpBuildLocalCaps(Codec, ies, sizeof(ies), &mct);
    return TSepFromCaps(Seid, InUse, caps, TCapsBytes(caps, ies, l, mct, Delay));
}

static AVDTP_REMOTE_SEP TSbcSinkSep(UCHAR Seid, UCHAR FreqMode, BOOLEAN InUse = FALSE)
{
    UCHAR ies[4] = { FreqMode, 0xff, 2, 53 }, caps[32];
    return TSepFromCaps(Seid, InUse, caps, TCapsBytes(caps, ies, 4, A2DP_CODEC_SBC, FALSE));
}

static PNA2DP_CONFIG TConfig()
{
    PNA2DP_CONFIG c;
    memset(&c, 0, sizeof(c));
    c.CodecPriority[0] = PNA2DP_CODEC_LDAC;
    c.CodecPriority[1] = PNA2DP_CODEC_AAC;
    c.CodecPriority[2] = PNA2DP_CODEC_SBC;
    c.EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC) |
                         PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX);
    c.ReconnectDelayMs = 1000;
    return c;
}

struct TChoiceEnv {
    AVDTP_LOCAL_SEP local[6];
    AVDTP_REMOTE_SEP remote[8];
    ULONG remoteCount;
    PNA2DP_CONFIG cfg;
    PN_REJECTED_SET rejected;
    PN_CODEC_CHOICE_INPUT in;
    TChoiceEnv() : remoteCount(0)
    {
        TBuildLocalSeps(local);
        cfg = TConfig();
        memset(&rejected, 0, sizeof(rejected));
        memset(&in, 0, sizeof(in));
        in.LocalSeps = local; in.LocalSepCount = 6;
        in.RemoteSeps = remote;
        in.Config = &cfg;
        in.PreferredRate = 48000; in.PreferredBits = 16;
        in.Rejected = &rejected;
    }
    void Add(const AVDTP_REMOTE_SEP &s) { remote[remoteCount++] = s; in.RemoteSepCount = remoteCount; }
    BOOLEAN Choose(PN_CODEC_CHOICE *c) { return PnChooseCodecConfig(&in, c); }
};

static PN_RECONCILE_INPUT TWant(AVDTP_STREAM_STATE St)
{
    PN_RECONCILE_INPUT in;
    memset(&in, 0, sizeof(in));
    in.AudioRun = TRUE; in.AudioStreamOpen = TRUE; in.StreamState = St; in.SignallingUp = St != AvdtpStateIdle;
    in.HostPresent = TRUE;
    return in;
}

static BOOLEAN TPlanIs(const PN_RECONCILE_PLAN &P, std::initializer_list<int> Steps)
{
    if (P.Count != Steps.size()) return FALSE;
    ULONG i = 0;
    for (int s : Steps) if (P.Steps[i++] != s) return FALSE;
    return TRUE;
}

static ULONG TPlanIndex(const PN_RECONCILE_PLAN &P, PN_RECONCILE_STEP Step)
{
    for (ULONG i = 0; i < P.Count; i++) if (P.Steps[i] == Step) return i;
    return 0xFFFFFFFFu;
}

static PN_SETUP_FAILURE TFail(PN_RECONCILE_STEP Step, LONG Status, ULONG SetupFailures = 0, ULONG Timeouts = 0, ULONG Attempts = 0)
{
    PN_FAILURE_INPUT f;
    memset(&f, 0, sizeof(f));
    f.Step = Step; f.Status = Status; f.ReconnectAttempts = Attempts; f.ReconnectDelayMs = 1000;
    f.SetupFailures = SetupFailures; f.ConsecutiveTimeouts = Timeouts;
    PN_SETUP_FAILURE a;
    PnReconcileOnFailure(&f, &a);
    return a;
}

/* Acceptor fixture with a fake device decision. */
struct TAcp {
    AVDTP_LOCAL_SEP local[6];
    UCHAR ies[32];
    AVDTP_ACP_CONTEXT ctx;
    AVDTP_ACP_RESULT r;
    static unsigned checkCalls;
    static UCHAR checkError;
    static UCHAR Check(PVOID, PAVDTP_LOCAL_SEP Local, UCHAR, const UCHAR *Ies, ULONG Len, ULONG, PUCHAR FailedCategory)
    {
        ++checkCalls;
        *FailedCategory = AVDTP_CAT_MEDIA_CODEC;
        UCHAR mct, dummy[32];
        A2dpBuildLocalCaps(Local->Codec, dummy, sizeof(dummy), &mct);
        A2DP_CODEC_CONFIG cfg;
        if (!A2dpParseConfig(mct, Ies, Len, &cfg)) return AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        return checkError;
    }
    static UCHAR CheckRe(PVOID, const UCHAR *, ULONG, PUCHAR FailedCategory) { ++checkCalls; *FailedCategory = AVDTP_CAT_MEDIA_CODEC; return checkError; }
    TAcp(AVDTP_STREAM_STATE St = AvdtpStateIdle, UCHAR LocalSeid = 0)
    {
        TBuildLocalSeps(local);
        memset(ies, 0, sizeof(ies));
        memset(&ctx, 0, sizeof(ctx));
        ctx.LocalSeps = local; ctx.LocalSepCount = 6;
        ctx.StreamState = St; ctx.LocalSeid = LocalSeid; ctx.RemoteSeid = LocalSeid ? 9 : 0;
        ctx.ConfiguredIes = ies;
        ctx.CheckConfiguration = Check; ctx.CheckReconfigure = CheckRe;
        checkCalls = 0; checkError = 0;
    }
    AVDTP_ACP_RESULT &Run(UCHAR Signal, std::initializer_list<UCHAR> Payload)
    {
        UCHAR p[64]; ULONG n = 0;
        for (UCHAR b : Payload) p[n++] = b;
        AvdtpAcpProcess(&ctx, Signal, p, n, &r);
        return r;
    }
    AVDTP_ACP_RESULT &Run(UCHAR Signal, const UCHAR *P, ULONG N) { AvdtpAcpProcess(&ctx, Signal, P, N, &r); return r; }
    BOOLEAN Rejected(UCHAR First, UCHAR Error) const
    {
        return r.MessageType == AVDTP_MSG_RESPONSE_REJECT && r.ResponseLength == 2 && r.Response[0] == First && r.Response[1] == Error;
    }
    BOOLEAN Rejected1(UCHAR Error) const
    {
        return r.MessageType == AVDTP_MSG_RESPONSE_REJECT && r.ResponseLength == 1 && r.Response[0] == Error;
    }
    BOOLEAN Accepted() const { return r.MessageType == AVDTP_MSG_RESPONSE_ACCEPT; }
};
unsigned TAcp::checkCalls;
UCHAR TAcp::checkError;

/* SET_CONFIGURATION payload: ACP SEID, INT SEID, then service capabilities. */
static ULONG TSetConfig(UCHAR *P, UCHAR AcpSeid, UCHAR IntSeid, const UCHAR *Caps, ULONG Len)
{
    P[0] = (UCHAR)(AcpSeid << 2); P[1] = (UCHAR)(IntSeid << 2);
    memcpy(P + 2, Caps, Len);
    return Len + 2;
}

/* SDP data elements */
static ULONG TSdpU16(UCHAR *P, USHORT V) { P[0] = 0x09; P[1] = (UCHAR)(V >> 8); P[2] = (UCHAR)V; return 3; }
static ULONG TSdpUuid16(UCHAR *P, USHORT V) { P[0] = 0x19; P[1] = (UCHAR)(V >> 8); P[2] = (UCHAR)V; return 3; }
static ULONG TSdpSeq(UCHAR *P, const UCHAR *Body, ULONG Len) { P[0] = 0x35; P[1] = (UCHAR)Len; memcpy(P + 2, Body, Len); return Len + 2; }

/* An AudioSink record: ProtocolDescriptorList (L2CAP PSM 0x19, AVDTP AvdtpVer),
 * profile descriptor (A2DP ProfileVer) and SupportedFeatures. */
static ULONG TSdpRecord(UCHAR *Out, USHORT AvdtpVer, USHORT ProfileVer, USHORT Features, BOOLEAN Wrap = TRUE)
{
    UCHAR a[16], b[16], l2[32], av[32], proto[64], pd[16], pdl[32], attrs[160];
    ULONG n, m, k = 0;
    n = TSdpUuid16(a, PN_SDP_UUID_L2CAP); n += TSdpU16(a + n, PN_SDP_PSM_AVDTP);
    ULONG l2n = TSdpSeq(l2, a, n);
    n = TSdpUuid16(b, PN_SDP_UUID_AVDTP); n += TSdpU16(b + n, AvdtpVer);
    ULONG avn = TSdpSeq(av, b, n);
    UCHAR both[64]; memcpy(both, l2, l2n); memcpy(both + l2n, av, avn);
    ULONG pn = TSdpSeq(proto, both, l2n + avn);
    n = TSdpUuid16(pd, PN_SDP_UUID_ADVANCED_AUDIO); n += TSdpU16(pd + n, ProfileVer);
    m = TSdpSeq(pdl, pd, n);
    UCHAR pdlOuter[40]; ULONG po = TSdpSeq(pdlOuter, pdl, m);
    k += TSdpU16(attrs + k, PN_SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST); memcpy(attrs + k, proto, pn); k += pn;
    k += TSdpU16(attrs + k, PN_SDP_ATTR_PROFILE_DESCRIPTOR_LIST); memcpy(attrs + k, pdlOuter, po); k += po;
    k += TSdpU16(attrs + k, PN_SDP_ATTR_SUPPORTED_FEATURES); k += TSdpU16(attrs + k, Features);
    if (!Wrap) return TSdpSeq(Out, attrs, k);
    UCHAR rec[200]; ULONG rn = TSdpSeq(rec, attrs, k);
    return TSdpSeq(Out, rec, rn);
}

/* Discovery against a scripted sink, using the same policy as AvdtpDiscover. The sink
 * answers GET_ALL_CAPABILITIES with AllStatus and GET_CAPABILITIES with its caps. */
struct TDiscoverySink {
    LONG allStatus;
    AVDTP_REMOTE_SEP seps[4];
    ULONG count;
};
static BOOLEAN TSimDiscover(const TDiscoverySink &Sink, USHORT Version, AVDTP_REMOTE_SEP *Out, ULONG *WithCaps)
{
    PN_CAPS_QUERY q;
    PnCapsQueryStart(&q, Version, FALSE);
    LONG status = 0;
    *WithCaps = 0;
    for (ULONG i = 0; i < Sink.count; i++) {   /* discovered, capabilities not known yet */
        Out[i] = Sink.seps[i];
        Out[i].CapsValid = FALSE; Out[i].Codec = PNA2DP_CODEC_NONE; Out[i].CodecCapsLength = 0;
    }
    for (ULONG i = 0; i < Sink.count; i++) {
        PN_CAPS_ACTION action = PnCapsNextSep;
        for (ULONG attempt = 0; attempt < 2; attempt++) {
            status = PnCapsQuerySignal(&q) == AVDTP_GET_ALL_CAPABILITIES ? Sink.allStatus : 0;
            action = PnCapsQueryResult(&q, status);
            if (action != PnCapsRetryBasic) break;
        }
        if (action == PnCapsStop) break;
        if (action != PnCapsParse) continue;
        Out[i] = Sink.seps[i];
        if (Out[i].CapsValid) ++*WithCaps;
    }
    return PnDiscoveryValid(status, Sink.count, *WithCaps);
}

/* ---- characterization: today's behaviour through the extracted seams ---------------- */

static void TestTChooser()
{
    PN_CODEC_CHOICE c;
    {
        TChoiceEnv e;
        e.Add(TSinkSep(1, PNA2DP_CODEC_SBC)); e.Add(TSinkSep(2, PNA2DP_CODEC_AAC)); e.Add(TSinkSep(3, PNA2DP_CODEC_LDAC));
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_LDAC && c.RemoteSeid == 3 && c.LocalSeid == 6 && c.Owner == PN_OWNER_HOST && c.RemoteIndex == 2);
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC);
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_AAC && c.RemoteSeid == 2 && c.LocalSeid == 2 && c.Owner == PN_OWNER_HOST);
        e.in.HostCodecMask = 0;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.RemoteSeid == 1 && c.LocalSeid == 1 && c.Owner == PN_OWNER_KERNEL_SBC);
        CHECK(c.Cfg.SampleRate == 48000);
        e.remote[1].InUse = 1; e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC);
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC);   /* a free SEP of a lower-priority codec wins */
        e.cfg.Flags = PNA2DP_CFG_HOST_CODECS_ONLY; e.in.HostCodecMask = 0;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.Owner == PN_OWNER_KERNEL_SBC);
    }
    {   /* SBC disabled: the last-resort loop still finds the SBC sink */
        TChoiceEnv e;
        e.Add(TSinkSep(4, PNA2DP_CODEC_SBC));
        e.cfg.EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC);
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.RemoteSeid == 4 && c.LocalSeid == 1 && c.Owner == PN_OWNER_KERNEL_SBC);
        e.in.LocalSepCount = 0;
        CHECK(e.Choose(&c) && c.LocalSeid == 1);   /* no local SBC SEP: SEID 1 */
    }
    {   /* an open stream restricts the rate */
        TChoiceEnv e;
        e.Add(TSbcSinkSep(1, 0x3f));
        e.in.RestrictRate = TRUE; e.in.PreferredRate = 44100;
        CHECK(e.Choose(&c) && c.Cfg.SampleRate == 44100);
        e.remote[0] = TSbcSinkSep(1, 0x1f);   /* 48 kHz only */
        CHECK(!e.Choose(&c));
        e.in.RestrictRate = FALSE; e.in.PreferredRate = 48000;
        CHECK(e.Choose(&c) && c.Cfg.SampleRate == 48000);
    }
    {   /* conservative radio: aptX cannot fit basic rate, SBC can */
        TChoiceEnv e;
        e.cfg.CodecPriority[0] = PNA2DP_CODEC_APTX; e.cfg.CodecPriority[1] = PNA2DP_CODEC_SBC; e.cfg.CodecPriority[2] = 0;
        e.Add(TSinkSep(1, PNA2DP_CODEC_APTX)); e.Add(TSinkSep(2, PNA2DP_CODEC_SBC));
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX);
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_APTX);
        e.in.Conservative = TRUE;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.Cfg.BitrateBps <= 350000);
    }
    {   /* source SEPs and SEPs without capabilities never match */
        TChoiceEnv e;
        AVDTP_REMOTE_SEP s = TSinkSep(1, PNA2DP_CODEC_SBC); s.Tsep = AVDTP_TSEP_SOURCE; e.Add(s);
        s = TSinkSep(2, PNA2DP_CODEC_SBC); s.CapsValid = FALSE; e.Add(s);
        CHECK(!e.Choose(&c));
    }
    UCHAR owner;
    CHECK(PnEncoderFor(PNA2DP_CODEC_SBC, 0, 0, &owner) && owner == PN_OWNER_KERNEL_SBC);
    CHECK(PnEncoderFor(PNA2DP_CODEC_SBC, PNA2DP_CFG_HOST_CODECS_ONLY, PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC), &owner) && owner == PN_OWNER_HOST);
    CHECK(PnEncoderFor(PNA2DP_CODEC_SBC, PNA2DP_CFG_HOST_CODECS_ONLY, 0, &owner) && owner == PN_OWNER_KERNEL_SBC);
    CHECK(!PnEncoderFor(PNA2DP_CODEC_LDAC, 0, 0, &owner) && owner == PN_OWNER_NONE);
    CHECK(!PnEncoderFor(40, 0, 0xffffffff, &owner));
    CHECK(PnRequestDelayReporting(PNA2DP_CFG_DELAY_REPORTING, PNA2DP_SVC_DELAY_REPORTING, 0x0103));
    CHECK(!PnRequestDelayReporting(PNA2DP_CFG_DELAY_REPORTING, PNA2DP_SVC_DELAY_REPORTING, 0x0102));
    CHECK(!PnRequestDelayReporting(0, PNA2DP_SVC_DELAY_REPORTING, 0x0103));
    CHECK(!PnRequestDelayReporting(PNA2DP_CFG_DELAY_REPORTING, 0, 0x0103));
    PNA2DP_CONFIG cfg = TConfig();
    CHECK(PnCodecRank(&cfg, PNA2DP_CODEC_LDAC) == 0 && PnCodecRank(&cfg, PNA2DP_CODEC_SBC) == 2 && PnCodecRank(&cfg, PNA2DP_CODEC_APTX) == 0xFFFFFFFFu);
    PN_REJECTED_SET rs = {};
    PnRejectedAdd(&rs, PNA2DP_CODEC_LDAC, 3); PnRejectedAdd(&rs, PNA2DP_CODEC_LDAC, 3);
    CHECK(rs.Count == 1 && PnRejectedContains(&rs, PNA2DP_CODEC_LDAC, 3) && !PnRejectedContains(&rs, PNA2DP_CODEC_LDAC, 4));
    for (UCHAR i = 0; i < 40; i++) PnRejectedAdd(&rs, PNA2DP_CODEC_SBC, i);
    CHECK(rs.Count == PN_REJECTED_MAX && !PnRejectedContains(NULL, 1, 1));
}

static void TestTPlanner()
{
    PN_RECONCILE_PLAN p;
    const std::initializer_list<int> setup = { PnStepEnsureSignalling, PnStepEnsureDiscovered, PnStepCheckFormat,
                                               PnStepEnsureOpen, PnStepStartIfOpen, PnStepStartPump };
    PN_RECONCILE_INPUT in = TWant(AvdtpStateIdle);
    PnPlanReconcile(&in, &p);
    CHECK(TPlanIs(p, setup) && p.RetryDelayMs == 0);
    in.Removing = TRUE; PnPlanReconcile(&in, &p); CHECK(p.Count == 0);
    in = TWant(AvdtpStateStreaming); in.PowerSuspended = TRUE; in.ResetBudget = TRUE;
    PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump }));
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.ResetBudget = TRUE; in.PowerResumePending = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(TPlanIs(p, { PnStepResetAttempts, PnStepResetSession, PnStepEnsureSignalling, PnStepEnsureDiscovered, PnStepCheckFormat,
                       PnStepEnsureOpen, PnStepStartIfOpen, PnStepStartPump }));
    /* the remote closed the stream under a running pump */
    in = TWant(AvdtpStateConfigured); in.PumpStreaming = TRUE;
    PnPlanReconcile(&in, &p);
    CHECK(p.Count == 7 && p.Steps[0] == PnStepStopPump && p.Steps[1] == PnStepEnsureSignalling);
    /* the codec host went away under its stream */
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.PumpOwnerHost = TRUE; in.HostPresent = FALSE;
    PnPlanReconcile(&in, &p);
    CHECK(p.Count == 8 && p.Steps[0] == PnStepStopPump && p.Steps[1] == PnStepCloseForHostLoss && p.Steps[2] == PnStepEnsureSignalling);
    in.HostPresent = TRUE; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
    /* hardware-validated SBC path: streaming and playing stays as it is */
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE;
    PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
    /* paused: keep streaming until the suspend timer */
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.AudioRun = FALSE;
    PnPlanReconcile(&in, &p); CHECK(p.Count == 0);
    in.SuspendDue = TRUE; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump, PnStepSuspend, PnStepNotifyState }));
    in.SuspendDue = FALSE; in.AudioStreamOpen = FALSE; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump, PnStepSuspend, PnStepNotifyState }));
    in = TWant(AvdtpStateOpen); in.PumpStreaming = TRUE; in.AudioRun = FALSE;
    PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump }));
    /* SCO blocks A2DP only with SUSPEND_ON_SCO */
    in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.ScoActive = TRUE;
    PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
    in.ConfigFlags = PNA2DP_CFG_SUSPEND_ON_SCO; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump, PnStepSuspend, PnStepNotifyState }));
    CHECK(PnPlanHas(&p, PnStepSuspend) && !PnPlanHas(&p, PnStepStartPump));

    /* failure handling */
    PN_SETUP_FAILURE f = TFail(PnStepEnsureSignalling, PN_ST_IO_TIMEOUT, 0, 0, 0);
    CHECK(f.NotifyError && f.BumpReconnectAttempts && f.RetryDelayMs == 1500 && !f.Disconnect);
    f = TFail(PnStepEnsureSignalling, PN_ST_IO_TIMEOUT, 0, 0, 10); CHECK(f.RetryDelayMs == 30000);
    /* step 2 (T4): every failure retries; a first timeout ABORTs instead of dropping signalling */
    f = TFail(PnStepEnsureDiscovered, PN_ST_NOT_SUPPORTED); CHECK(f.NotifyError && f.RetryDelayMs == 1500 && !f.Disconnect && !f.AbortStream);
    f = TFail(PnStepEnsureOpen, PN_ST_IO_TIMEOUT); CHECK(f.NotifyError && f.AbortStream && !f.Disconnect && f.RetryDelayMs == 1500);
    f = TFail(PnStepEnsureOpen, PN_ST_CONNECTION_DISCONNECTED); CHECK(f.Disconnect && f.RetryDelayMs == 1500);
    f = TFail(PnStepStartPump, PN_ST_DEVICE_NOT_READY); CHECK(f.NotifyError && f.SuspendIfStreaming);
    f = TFail(PnStepStartIfOpen, PN_ST_DEVICE_PROTOCOL_ERROR); CHECK(f.NotifyError && !f.SuspendIfStreaming && !f.Disconnect);

    CHECK(PnSetConfigFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, 0, TRUE, FALSE) == PnSetConfigRetryWithoutDelay);
    CHECK(PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, 0, FALSE, 0) == PnOpenFailAbort);
    CHECK(PnOpenFailureAction(PN_ST_NOT_SUPPORTED, 0, FALSE, 0) == PnOpenFailAbort);
    CHECK(PnOpenFailureAction(PN_ST_IO_TIMEOUT, 0, FALSE, 0) == PnOpenFailKeep);
    CHECK(PnStartRejectIsCollision(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, AvdtpStateStreaming));
    CHECK(!PnStartRejectIsCollision(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, AvdtpStateOpen));
    CHECK(!PnStartRejectIsCollision(PN_ST_IO_TIMEOUT, 0, AvdtpStateStreaming));
    CHECK(PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, TRUE, AvdtpStateIdle));
    CHECK(PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, FALSE, AvdtpStateOpening));
    CHECK(!PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, FALSE, AvdtpStateOpen));
    CHECK(!PnAcceptIncomingAsMedia(TRUE, TRUE, FALSE, TRUE, AvdtpStateOpening));
    CHECK(!PnAcceptIncomingAsMedia(TRUE, FALSE, TRUE, TRUE, AvdtpStateOpening));
    CHECK(!PnAcceptIncomingAsMedia(FALSE, FALSE, FALSE, TRUE, AvdtpStateOpening));
    CHECK(PnStreamCloseAction(AvdtpStateIdle) == PnCloseNothing);
    CHECK(PnStreamCloseAction(AvdtpStateOpen) == PnCloseSendClose && PnStreamCloseAction(AvdtpStateStreaming) == PnCloseSendClose);
    CHECK(PnStreamCloseAction(AvdtpStateClosing) == PnCloseLocalOnly);
    CHECK(PnStatusState(AvdtpStateIdle, FALSE, FALSE) == PNA2DP_STATE_IDLE);
    CHECK(PnStatusState(AvdtpStateIdle, TRUE, FALSE) == PNA2DP_STATE_DISCOVERING);
    CHECK(PnStatusState(AvdtpStateOpening, TRUE, FALSE) == PNA2DP_STATE_CONFIGURED);
    CHECK(PnStatusState(AvdtpStateOpen, TRUE, FALSE) == PNA2DP_STATE_OPEN);
    CHECK(PnStatusState(AvdtpStateStreaming, TRUE, FALSE) == PNA2DP_STATE_STREAMING);
    CHECK(PnStatusState(AvdtpStateAborting, TRUE, TRUE) == PNA2DP_STATE_CLOSING);
    CHECK(PnPumpMayRun(AvdtpStateStreaming) && !PnPumpMayRun(AvdtpStateConfigured) && !PnPumpMayRun(AvdtpStateIdle));
}

static void TestTFormat()
{
    PNA2DP_CONFIG cfg = TConfig();
    AVDTP_REMOTE_SEP sbc = TSbcSinkSep(1, 0x3f), sbc44 = TSbcSinkSep(1, 0x2f);
    UCHAR offer44[] = { 0x21, 0x15, 2, 53 }, offer48[] = { 0x11, 0x15, 2, 53 };
    A2DP_CODEC_CONFIG active, out;
    CHECK(A2dpParseConfig(A2DP_CODEC_SBC, offer44, 4, &active));
    PN_FORMAT_INPUT in;
    memset(&in, 0, sizeof(in));
    in.ActiveValid = TRUE; in.Active = &active; in.Remote = &sbc; in.Config = &cfg; in.AudioStreamOpen = TRUE;
    in.AudioRate = 48000; in.AudioValidBits = 16; in.StreamState = AvdtpStateOpen;
    CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000 && out.Codec == PNA2DP_CODEC_SBC);
    in.AudioRate = 44100; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    in.AudioRate = 48000; in.Remote = &sbc44; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReselect);
    in.Remote = NULL; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReselect);
    in.Remote = &sbc; in.StreamState = AvdtpStateIdle; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    in.StreamState = AvdtpStateOpening; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    in.StreamState = AvdtpStateConfigured; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure);
    in.ActiveValid = FALSE; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    in.ActiveValid = TRUE; in.AudioStreamOpen = FALSE; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    /* a joint-stereo stream the receiver chose: dual channel preference reconfigures */
    CHECK(A2dpParseConfig(A2DP_CODEC_SBC, offer48, 4, &active));
    in.AudioStreamOpen = TRUE; in.StreamState = AvdtpStateStreaming;
    CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    cfg.Flags = PNA2DP_CFG_DUAL_CHANNEL_PREFER;
    CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.ChannelMode == PNA2DP_CHMODE_DUAL);
    in.Conservative = TRUE;   /* basic rate cannot carry dual channel at this bitpool */
    PN_FORMAT_ACTION a = PnPlanStreamFormat(&in, &out);
    CHECK(a == PnFormatKeep || a == PnFormatReconfigure);
    /* AAC: the rate follows the engine; mode changes do not reconfigure */
    {
        AVDTP_REMOTE_SEP aac = TSinkSep(2, PNA2DP_CODEC_AAC);
        A2DP_CODEC_CAPS caps; A2DP_CODEC_CONFIG aacActive;
        PNA2DP_CONFIG pref = {};
        CHECK(A2dpParseCaps(aac.MediaCodecType, aac.CodecCaps, aac.CodecCapsLength, &caps, NULL, NULL));
        CHECK(A2dpSelectConfig(&caps, &pref, 44100, 16, &aacActive));
        in.Active = &aacActive; in.Remote = &aac; in.Conservative = FALSE;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000 && out.Codec == PNA2DP_CODEC_AAC);
        in.AudioRate = 44100; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    }
}

static void TestTAcceptor()
{
    {   /* DISCOVER */
        TAcp a;
        a.Run(AVDTP_DISCOVER, {});
        CHECK(a.Accepted() && a.r.ResponseLength == 12 && a.r.Effects == 0);
        CHECK(a.r.Response[0] == (1 << 2) && a.r.Response[1] == 0x00 && a.r.Response[10] == (6 << 2));
        TAcp b(AvdtpStateConfigured, 1);
        b.Run(AVDTP_DISCOVER, {});
        CHECK(b.r.Response[0] == ((1 << 2) | 0x02) && b.r.Response[2] == (2 << 2));
    }
    {   /* GET_(ALL_)CAPABILITIES */
        TAcp a;
        a.Run(AVDTP_GET_CAPABILITIES, { 1 << 2 });
        CHECK(a.Accepted() && a.r.ResponseLength == 10 && a.r.Response[0] == AVDTP_CAT_MEDIA_TRANSPORT && a.r.Response[2] == AVDTP_CAT_MEDIA_CODEC && a.r.Response[5] == A2DP_CODEC_SBC);
        a.Run(AVDTP_GET_ALL_CAPABILITIES, { 1 << 2 });
        CHECK(a.Accepted() && a.r.ResponseLength == 12 && a.r.Response[10] == AVDTP_CAT_DELAY_REPORTING);
        a.Run(AVDTP_GET_CAPABILITIES, { 9 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_ACP_SEID));
        a.Run(AVDTP_GET_CAPABILITIES, {}); CHECK(a.Rejected1(AVDTP_ERR_BAD_LENGTH));
        a.Run(AVDTP_GET_ALL_CAPABILITIES, { 6 << 2 }); CHECK(a.Accepted() && a.r.Response[5] == A2DP_CODEC_VENDOR);
    }
    UCHAR sbc44[] = { 0x21, 0x15, 2, 53 }, caps[40], p[64];
    {   /* SET_CONFIGURATION */
        TAcp a;
        ULONG n = TSetConfig(p, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, TRUE));
        a.Run(AVDTP_SET_CONFIGURATION, p, n);
        CHECK(a.Accepted() && a.r.ResponseLength == 0 && TAcp::checkCalls == 1);
        /* step 2 (T6): a new configuration also resets the delay report */
        CHECK(a.r.Effects == (AVDTP_ACP_FX_STORE_CONFIG | AVDTP_ACP_FX_RESET_DELAY | AVDTP_ACP_FX_NEW_STATE) && a.r.NewState == AvdtpStateConfigured);
        CHECK(a.r.ConfigLocalSeid == 1 && a.r.ConfigRemoteSeid == 5 && a.r.IesLength == 4 && memcmp(a.r.Ies, sbc44, 4) == 0);
        CHECK(a.r.DelayReporting && (a.r.ServiceCaps & PNA2DP_SVC_MEDIA_TRANSPORT));
        n = TSetConfig(p, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, FALSE));
        a.Run(AVDTP_SET_CONFIGURATION, p, n); CHECK(a.Accepted() && !a.r.DelayReporting);
        TAcp::checkError = A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE;
        a.Run(AVDTP_SET_CONFIGURATION, p, n); CHECK(a.Rejected(AVDTP_CAT_MEDIA_CODEC, A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE) && a.r.Effects == 0);
        TAcp::checkError = 0;
        unsigned calls = TAcp::checkCalls;
        n = TSetConfig(p, 2, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, FALSE));   /* SBC config to the AAC SEP */
        a.Run(AVDTP_SET_CONFIGURATION, p, n);
        CHECK(a.Rejected(AVDTP_CAT_MEDIA_CODEC, AVDTP_ERR_UNSUPPORTED_CONFIGURATION) && TAcp::checkCalls == calls);
        n = TSetConfig(p, 9, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, FALSE));
        a.Run(AVDTP_SET_CONFIGURATION, p, n); CHECK(a.Rejected(0, AVDTP_ERR_BAD_ACP_SEID));
        a.Run(AVDTP_SET_CONFIGURATION, { 1 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_LENGTH));
        TAcp b(AvdtpStateOpen, 1);
        n = TSetConfig(p, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, FALSE));
        b.Run(AVDTP_SET_CONFIGURATION, p, n); CHECK(b.Rejected(0, AVDTP_ERR_SEP_IN_USE));
    }
    {   /* GET_CONFIGURATION / RECONFIGURE */
        TAcp a(AvdtpStateOpen, 1);
        memcpy(a.ies, sbc44, 4); a.ctx.ConfiguredIesLength = 4; a.ctx.DelayReportingNegotiated = TRUE;
        a.Run(AVDTP_GET_CONFIGURATION, { 1 << 2 });
        CHECK(a.Accepted() && a.r.ResponseLength == 12 && memcmp(a.r.Response + 6, sbc44, 4) == 0 && a.r.Response[10] == AVDTP_CAT_DELAY_REPORTING);
        a.Run(AVDTP_GET_CONFIGURATION, { 2 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_ACP_SEID));
        UCHAR re[16]; ULONG n = 0;
        re[n++] = 9 << 2;
        n += AvdtpBuildMediaCodecCap(re + n, sizeof(re) - n, A2DP_CODEC_SBC, sbc44, 4);
        a.Run(AVDTP_RECONFIGURE, re, n);
        CHECK(a.Accepted() && a.r.Effects == AVDTP_ACP_FX_STORE_IES && a.r.IesLength == 4);
        TAcp::checkError = A2DP_ERR_NOT_SUPPORTED_SAMPLING_FREQUENCY;
        a.Run(AVDTP_RECONFIGURE, re, n); CHECK(a.Rejected(AVDTP_CAT_MEDIA_CODEC, A2DP_ERR_NOT_SUPPORTED_SAMPLING_FREQUENCY));
        TAcp b(AvdtpStateConfigured, 1);
        b.Run(AVDTP_RECONFIGURE, re, n); CHECK(b.Rejected(0, AVDTP_ERR_BAD_STATE));
    }
    {   /* OPEN / START / SUSPEND / CLOSE / ABORT */
        TAcp a(AvdtpStateConfigured, 1);
        a.Run(AVDTP_OPEN, { 1 << 2 });
        CHECK(a.Accepted() && a.r.Effects == (AVDTP_ACP_FX_MEDIA_EXPECTED | AVDTP_ACP_FX_NEW_STATE) && a.r.NewState == AvdtpStateOpening);
        a.Run(AVDTP_START, { 1 << 2 }); CHECK(a.Rejected(1 << 2, AVDTP_ERR_BAD_STATE));
        a.Run(AVDTP_CLOSE, { 1 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_STATE));
        a.Run(AVDTP_ABORT, { 1 << 2 });
        CHECK(a.Accepted() && a.r.MidState == AvdtpStateAborting && a.r.NewState == AvdtpStateIdle &&
              (a.r.Effects & (AVDTP_ACP_FX_MID_STATE | AVDTP_ACP_FX_DISCONNECT_MEDIA | AVDTP_ACP_FX_NEW_STATE)) == (AVDTP_ACP_FX_MID_STATE | AVDTP_ACP_FX_DISCONNECT_MEDIA | AVDTP_ACP_FX_NEW_STATE));
        a.ctx.StreamState = AvdtpStateOpen;
        a.Run(AVDTP_START, { 1 << 2 }); CHECK(a.Accepted() && (a.r.Effects & AVDTP_ACP_FX_NEW_STATE) && a.r.NewState == AvdtpStateStreaming);
        a.Run(AVDTP_SUSPEND, { 1 << 2 }); CHECK(a.Rejected(1 << 2, AVDTP_ERR_BAD_STATE));
        a.Run(AVDTP_OPEN, { 1 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_STATE));
        a.Run(AVDTP_CLOSE, { 1 << 2 });
        CHECK(a.Accepted() && a.r.MidState == AvdtpStateClosing && a.r.NewState == AvdtpStateIdle && (a.r.Effects & AVDTP_ACP_FX_DISCONNECT_MEDIA));
        a.ctx.StreamState = AvdtpStateStreaming;
        a.Run(AVDTP_SUSPEND, { 1 << 2 }); CHECK(a.Accepted() && (a.r.Effects & AVDTP_ACP_FX_NEW_STATE) && a.r.NewState == AvdtpStateOpen);
        a.Run(AVDTP_SUSPEND, {}); CHECK(a.Rejected1(AVDTP_ERR_BAD_LENGTH));
        a.ctx.StreamState = AvdtpStateIdle;
        a.Run(AVDTP_ABORT, { 1 << 2 }); CHECK(a.Accepted() && a.r.Effects == 0);
    }
    {   /* DELAYREPORT, SECURITY_CONTROL, unknown */
        TAcp a(AvdtpStateOpen, 1);
        a.Run(AVDTP_DELAYREPORT, { 1 << 2, 0x05, 0xDC });
        CHECK(a.Accepted() && a.r.DelayTenthsMs == 1500 && a.r.Effects == (AVDTP_ACP_FX_STORE_DELAY | AVDTP_ACP_FX_DELAY_CALLBACK));
        a.Run(AVDTP_DELAYREPORT, { 1 << 2, 0x05 }); CHECK(a.Rejected1(AVDTP_ERR_BAD_LENGTH));
        a.Run(AVDTP_SECURITY_CONTROL, { 1 << 2 }); CHECK(a.Rejected1(AVDTP_ERR_NOT_SUPPORTED_COMMAND));
        a.Run(0x3F, {}); CHECK(a.r.MessageType == AVDTP_MSG_GENERAL_REJECT && a.r.ResponseLength == 0);
    }
}

static void TestTDiscoveryAndSdp()
{
    UCHAR d[] = { (1 << 2) | 0x02, 0x08, (2 << 2), 0x10, (3 << 2), 0x00 };   /* audio sink in use, video, audio source */
    AVDTP_REMOTE_SEP seps[4];
    CHECK(AvdtpParseDiscover(d, sizeof(d), seps, 4) == 2);
    CHECK(seps[0].Seid == 1 && seps[0].InUse && seps[0].Tsep == AVDTP_TSEP_SINK && seps[1].Seid == 3 && seps[1].Tsep == AVDTP_TSEP_SOURCE);
    CHECK(AvdtpParseDiscover(d, sizeof(d), seps, 1) == 1 && AvdtpParseDiscover(d, 1, seps, 4) == 0);

    PN_CAPS_QUERY q;
    PnCapsQueryStart(&q, 0x0103, FALSE);
    CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_ALL_CAPABILITIES);
    CHECK(PnCapsQueryResult(&q, 0) == PnCapsParse);
    CHECK(PnCapsQueryResult(&q, PN_ST_NOT_SUPPORTED) == PnCapsRetryBasic && q.Version == 0x0102 && q.Downgraded);
    CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES && PnCapsQueryResult(&q, PN_ST_NOT_SUPPORTED) == PnCapsNextSep);
    PnCapsQueryStart(&q, 0x0102, FALSE); CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES);
    PnCapsQueryStart(&q, 0x0103, TRUE); CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES);
    PnCapsQueryStart(&q, 0x0102, FALSE);
    CHECK(PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsStop && PnCapsQueryResult(&q, PN_ST_CONNECTION_DISCONNECTED) == PnCapsStop);
    CHECK(PnCapsQueryResult(&q, PN_ST_DEVICE_PROTOCOL_ERROR) == PnCapsNextSep);
    CHECK(!PnDiscoveryValid(0, 0, 0) && PnDiscoveryValid(0, 2, 2));   /* step 2 (T5): no SEP is not a discovery */

    UCHAR rec[256];
    PN_SDP_A2DP_INFO info;
    ULONG n = TSdpRecord(rec, 0x0103, 0x0104, 0x000F);
    CHECK(PnSdpParseA2dpRecord(rec, n, &info));
    CHECK(info.ProfileVersionFound && info.ProfileVersion == 0x0104 && info.FeaturesSeen && info.SupportedFeatures == 0x000F);
    CHECK(info.AvdtpVersionFound && info.AvdtpVersion == 0x0103 && info.L2capPsmAvdtp);
    CHECK(PnSdpAvdtpVersionToUse(&info) == 0x0103);   /* step 2 (T5): the AVDTP version, not the A2DP profile's */
    n = TSdpRecord(rec, 0x0102, 0x0103, 0x0001, FALSE);   /* bare attribute list */
    CHECK(PnSdpParseA2dpRecord(rec, n, &info) && info.AvdtpVersion == 0x0102 && info.ProfileVersion == 0x0103);
    for (ULONG len = 0; len < n; ++len) { PnSdpParseA2dpRecord(rec, len, &info); CHECK(info.ProfileVersion == 0 || info.ProfileVersion == 0x0103); }
    UCHAR notSeq[] = { 0x09, 0x01, 0x00 };
    CHECK(!PnSdpParseA2dpRecord(notSeq, sizeof(notSeq), &info) && !info.ProfileVersionFound);
    UCHAR uuid128[] = { 0x1C, 0x00, 0x00, 0x00, 0x19, 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB };
    USHORT u;
    CHECK(PnSdpUuid16(uuid128[0] >> 3, uuid128 + 1, 16, &u) && u == PN_SDP_UUID_AVDTP);
    uuid128[16] = 0;
    CHECK(!PnSdpUuid16(uuid128[0] >> 3, uuid128 + 1, 16, &u));
    UCHAR uuid32[] = { 0x00, 0x00, 0x01, 0x00 };
    CHECK(PnSdpUuid16(PN_SDP_TYPE_UUID, uuid32, 4, &u) && u == PN_SDP_UUID_L2CAP);
    /* malformed SupportedFeatures: seen, value 0 (as the old parser stored it) */
    UCHAR badFeat[] = { 0x35, 0x05, 0x09, 0x03, 0x11, 0x08, 0x07 };
    CHECK(PnSdpParseA2dpRecord(badFeat, sizeof(badFeat), &info) && info.FeaturesSeen && info.SupportedFeatures == 0);
    ULONG random = 0x5d9a2b11;
    for (unsigned i = 0; i < 20000; ++i) {
        UCHAR buf[96];
        ULONG size = 1 + (random % 95);
        for (ULONG j = 0; j < size; ++j) { random ^= random << 13; random ^= random >> 17; random ^= random << 5; buf[j] = (UCHAR)random; }
        if (i & 1) buf[0] = 0x35, buf[1] = (UCHAR)(size - 2);
        PnSdpParseA2dpRecord(buf, size, &info);
    }
}

static void TestTSmallPolicies()
{
    /* MTU (step 2, T12): unknown radio -> one 3-DH5 */
    CHECK(PnMediaMtuCap(0, 0) == 1017 && PnPreferredMediaMtu(0, 0) == 1017);
    CHECK(PnEffectiveMediaMtu(895, PN_MEDIA_MTU_NO_CAP, 672) == 895 && PnEffectiveMediaMtu(0, 1017, 672) == 672);
    CHECK(PnEffectiveMediaMtu(1024, 1017, 672) == 1017);
    /* setup retry back-off */
    PnSetupRetry r = {};
    CHECK(PnSetupRetryFailed(&r) == 1500 && PnSetupRetryFailed(&r) == 3000 && PnSetupRetryFailed(&r) == 6000);
    CHECK(PnSetupRetryFailed(&r) == 12000 && PnSetupRetryFailed(&r) == 24000 && PnSetupRetryFailed(&r) == 30000);
    for (unsigned i = 0; i < 40; ++i) CHECK(PnSetupRetryFailed(&r) == 30000);
    PnSetupRetryObserve(&r, 1000, 1); PnSetupRetryObserve(&r, 20000, 1); CHECK(r.failures != 0);
    PnSetupRetryObserve(&r, 25000, 0); PnSetupRetryObserve(&r, 26000, 1); PnSetupRetryObserve(&r, 50000, 1); CHECK(r.failures != 0);
    PnSetupRetryObserve(&r, 56000, 1); CHECK(r.failures == 0 && PnSetupRetryFailed(&r) == 1500);
    CHECK(PnSetupRetryDelayMs(0xffffffffu) == 30000);
    /* power: system sleep drops the session on resume (unchanged) */
    CHECK(PnPowerOnDx(FALSE) == (PN_PWR_BLOCK_WORK | PN_PWR_RESET_ON_RESUME | PN_PWR_CANCEL_RECONNECT | PN_PWR_QUEUE_WORK));
    CHECK(PnPowerResetOnResume(TRUE, FALSE, FALSE) && !PnPowerResetOnResume(FALSE, TRUE, TRUE));
    /* ACL */
    CHECK(PnAclEventAction(TRUE, FALSE) == (PN_ACL_FX_RESET_BUDGET | PN_ACL_FX_RECONNECT_SOON));
    CHECK(PnAclEventAction(TRUE, TRUE) == PN_ACL_FX_RESET_BUDGET);
    /* sink delay */
    CHECK(PnSinkDelayUs(TRUE, TRUE, TRUE, 1500, PNA2DP_CODEC_SBC) == 150000);
    CHECK(PnDefaultSinkDelayUs(PNA2DP_CODEC_APTX_LL) < PnDefaultSinkDelayUs(PNA2DP_CODEC_SBC));
    CHECK(PnDefaultSinkDelayUs(PNA2DP_CODEC_NONE) == PN_SINK_DELAY_DEFAULT_US && PnDefaultSinkDelayUs(PNA2DP_CODEC_LDAC) == PN_SINK_DELAY_LDAC_US);
    /* capability parsing records CP types */
    UCHAR cp[] = { 1,0, 4,2,0x02,0x00, 7,6,0,0, 0x11,0x15,2,53 };
    AVDTP_REMOTE_SEP s = {};
    CHECK(AvdtpParseServiceCapabilities(cp, sizeof(cp), &s) && s.CpTypeMask == AVDTP_CP_MASK_SCMS_T && (s.ServiceCaps & PNA2DP_SVC_CONTENT_PROTECTION));
    cp[4] = 0x01; CHECK(AvdtpParseServiceCapabilities(cp, sizeof(cp), &s) && s.CpTypeMask == AVDTP_CP_MASK_DTCP);
    cp[4] = 0x07; CHECK(AvdtpParseServiceCapabilities(cp, sizeof(cp), &s) && s.CpTypeMask == AVDTP_CP_MASK_OTHER);
}

/* ---- characterization: differential against the pre-refactor driver code -------------
 * The TOld* functions are the code removed from device.cpp, avdtp.cpp and bthlink.cpp,
 * transcribed with I/O replaced by a log. Randomized inputs must give identical results. */

static ULONG tRand = 0x7a3c19e5;
static ULONG TRnd() { tRand ^= tRand << 13; tRand ^= tRand >> 17; tRand ^= tRand << 5; return tRand; }

/* old device.cpp ChooseConfig / EncoderAvailable / LocalSepForCodec */
static BOOLEAN TOldEncoderAvailable(const PNA2DP_CONFIG *Config, ULONG HostMask, ULONG Codec, UCHAR *Owner)
{
    if (Codec == PNA2DP_CODEC_SBC && !(Config->Flags & PNA2DP_CFG_HOST_CODECS_ONLY)) { *Owner = PN_OWNER_KERNEL_SBC; return TRUE; }
    if (Codec < 32 && (HostMask & PNA2DP_CODEC_BIT(Codec))) { *Owner = PN_OWNER_HOST; return TRUE; }
    if (Codec == PNA2DP_CODEC_SBC) { *Owner = PN_OWNER_KERNEL_SBC; return TRUE; }
    return FALSE;
}
static const AVDTP_LOCAL_SEP *TOldLocalSepForCodec(const AVDTP_LOCAL_SEP *L, ULONG LC, ULONG Codec)
{
    for (ULONG i = 0; i < LC; i++) if (L[i].Codec == Codec) return &L[i];
    return NULL;
}
static const AVDTP_REMOTE_SEP *TOldChooseConfig(const AVDTP_REMOTE_SEP *RemoteSeps, ULONG RemoteSepCount, const AVDTP_LOCAL_SEP *L, ULONG LC,
                                                const PNA2DP_CONFIG *Config, ULONG HostMask, BOOLEAN AudioStreamOpen, ULONG AudioRate,
                                                ULONG AudioValidBits, BOOLEAN conservative, PA2DP_CODEC_CONFIG Cfg, UCHAR *Owner, PUCHAR LocalSeid)
{
    ULONG prefRate = AudioStreamOpen ? AudioRate : 48000;
    ULONG prefBits = AudioStreamOpen ? AudioValidBits : 16;
    for (ULONG pi = 0; pi < PN_ARRAYSIZE(Config->CodecPriority); pi++) {
        ULONG codec = Config->CodecPriority[pi];
        if (!codec) break;
        if (!(Config->EnabledCodecMask & PNA2DP_CODEC_BIT(codec))) continue;
        UCHAR owner;
        if (!TOldEncoderAvailable(Config, HostMask, codec, &owner)) continue;
        const AVDTP_LOCAL_SEP *local = TOldLocalSepForCodec(L, LC, codec);
        if (!local) continue;
        for (ULONG i = 0; i < RemoteSepCount; i++) {
            const AVDTP_REMOTE_SEP *r = &RemoteSeps[i];
            if (!r->CapsValid || r->Codec != codec || r->Tsep != AVDTP_TSEP_SINK || r->InUse) continue;
            A2DP_CODEC_CAPS caps;
            if (!A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL)) continue;
            if (AudioStreamOpen) caps.RateMask &= A2dpHzToRateMask(prefRate);
            if (A2dpSelectConfig(&caps, Config, prefRate, prefBits, Cfg)) {
                if (conservative && !A2dpConstrainBasicRadio(Cfg)) continue;
                *Owner = owner;
                *LocalSeid = local->Seid;
                return r;
            }
        }
    }
    for (ULONG i = 0; i < RemoteSepCount; i++) {
        const AVDTP_REMOTE_SEP *r = &RemoteSeps[i];
        if (!r->CapsValid || r->Codec != PNA2DP_CODEC_SBC || r->Tsep != AVDTP_TSEP_SINK || r->InUse) continue;
        A2DP_CODEC_CAPS caps;
        if (!A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL)) continue;
        if (AudioStreamOpen) caps.RateMask &= A2dpHzToRateMask(prefRate);
        if (A2dpSelectConfig(&caps, Config, prefRate, prefBits, Cfg)) {
            if (conservative && !A2dpConstrainBasicRadio(Cfg)) continue;
            const AVDTP_LOCAL_SEP *local = TOldLocalSepForCodec(L, LC, PNA2DP_CODEC_SBC);
            *Owner = PN_OWNER_KERNEL_SBC;
            *LocalSeid = local ? local->Seid : 1;
            return r;
        }
    }
    return NULL;
}

/* old device.cpp Reconcile format block: 0 keep, 1 reconfigure (Cfg), 2 reselect */
static int TOldFormat(BOOLEAN ActiveValid, BOOLEAN AudioStreamOpen, AVDTP_STREAM_STATE st, const A2DP_CODEC_CONFIG *Active,
                      const AVDTP_REMOTE_SEP *r, const PNA2DP_CONFIG *Config, ULONG AudioRate, ULONG AudioValidBits, BOOLEAN conservative,
                      A2DP_CODEC_CONFIG *Out)
{
    if (ActiveValid && AudioStreamOpen && (st == AvdtpStateOpen || st == AvdtpStateStreaming || st == AvdtpStateConfigured)) {
        A2DP_CODEC_CONFIG cfg = *Active;
        A2DP_CODEC_CAPS caps;
        BOOLEAN selected = r && A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL) && (caps.RateMask & A2dpHzToRateMask(AudioRate));
        if (selected && cfg.Codec == PNA2DP_CODEC_SBC) {
            caps.RateMask &= A2dpHzToRateMask(AudioRate);
            selected = A2dpSelectConfig(&caps, Config, AudioRate, AudioValidBits, &cfg);
            if (selected && conservative) selected = A2dpConstrainBasicRadio(&cfg);
        } else if (selected) {
            cfg.SampleRate = AudioRate;
            A2dpSerializeConfig(&cfg);
        }
        if (selected && PnStreamFormatNeedsUpdate(Active->Codec, Active->SampleRate, Active->ChannelMode, cfg.SampleRate, cfg.ChannelMode)) { *Out = cfg; return 1; }
        if (!selected && Active->SampleRate != AudioRate) return 2;
    }
    return 0;
}

/* old bthlink.cpp SDP parser, writing into a fake link */
struct TOldLink { USHORT RemoteAvdtpVersion, RemoteSupportedFeatures; };
static void TOldSdpParseProfileList(TOldLink *Link, const UCHAR *P, ULONG Len)
{
    while (Len) {
        UCHAR t; const UCHAR *d; ULONG dl, tot;
        if (!PnSdpElement(P, Len, &t, &d, &dl, &tot)) return;
        if (t == PN_SDP_TYPE_SEQUENCE) {
            UCHAR t1, t2; const UCHAR *d1, *d2; ULONG l1, l2, n1, n2;
            USHORT uuid, ver;
            if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && PnSdpUint16(t1, d1, l1, &uuid) && uuid == 0x110D &&
                PnSdpElement(d + n1, dl - n1, &t2, &d2, &l2, &n2) && PnSdpUint16(t2, d2, l2, &ver)) {
                Link->RemoteAvdtpVersion = ver;
                return;
            }
        }
        P += tot; Len -= tot;
    }
}
static BOOLEAN TOldSdpParseRecord(TOldLink *Link, const UCHAR *P, ULONG Len)
{
    UCHAR t; const UCHAR *d; ULONG dl, tot;
    if (!PnSdpElement(P, Len, &t, &d, &dl, &tot) || t != PN_SDP_TYPE_SEQUENCE) return FALSE;
    {
        UCHAR t1; const UCHAR *d1; ULONG l1, n1;
        if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && t1 == PN_SDP_TYPE_SEQUENCE) { d = d1; dl = l1; }
    }
    while (dl) {
        UCHAR ti, tv; const UCHAR *di, *dv; ULONG li, lv, ni, nv;
        USHORT id;
        if (!PnSdpElement(d, dl, &ti, &di, &li, &ni) || !PnSdpUint16(ti, di, li, &id)) break;
        if (!PnSdpElement(d + ni, dl - ni, &tv, &dv, &lv, &nv)) break;
        if (id == 0x0009 && tv == PN_SDP_TYPE_SEQUENCE) TOldSdpParseProfileList(Link, dv, lv);
        else if (id == 0x0311) PnSdpUint16(tv, dv, lv, &Link->RemoteSupportedFeatures);
        d += ni + nv; dl -= ni + nv;
    }
    return TRUE;
}

/* old avdtp.cpp AvdtpProcessCommand on a fake session; I/O becomes a log */
struct TSess {
    AVDTP_LOCAL_SEP LocalSeps[6];
    ULONG LocalSepCount;
    AVDTP_STREAM_STATE StreamState;
    UCHAR LocalSeid, RemoteSeid, ConfiguredIes[32], ConfiguredIesLength;
    ULONG ConfiguredServiceCaps, SinkDelayTenthsMs;
    BOOLEAN DelayReportingNegotiated, MediaExpected, MediaUp;
    UCHAR log[1024];
    ULONG logLen;
    void Log(UCHAR b) { if (logLen < sizeof(log)) log[logLen++] = b; }
    void Send(UCHAR type, const UCHAR *p, ULONG n) { Log(0xA0); Log(type); Log((UCHAR)n); for (ULONG i = 0; i < n; i++) Log(p[i]); }
    void SetState(AVDTP_STREAM_STATE s) { if (StreamState != s) { Log(0xB0); Log((UCHAR)s); } StreamState = s; }
    void Disconnect() { Log(0xC0); }
    void Delay(ULONG d) { Log(0xD0); Log((UCHAR)d); Log((UCHAR)(d >> 8)); }
};
static PAVDTP_LOCAL_SEP TOldFindLocalSep(TSess *S, UCHAR Seid)
{
    for (ULONG i = 0; i < S->LocalSepCount; i++) if (S->LocalSeps[i].Seid == Seid) return &S->LocalSeps[i];
    return NULL;
}
static ULONG TOldBuildLocalCapabilities(PAVDTP_LOCAL_SEP Sep, BOOLEAN All, PUCHAR Out, ULONG Cap)
{
    ULONG n = 0;
    if (Cap < 8) return 0;
    Out[n++] = AVDTP_CAT_MEDIA_TRANSPORT; Out[n++] = 0;
    UCHAR mct;
    UCHAR ies[32];
    ULONG l = A2dpBuildLocalCaps(Sep->Codec, ies, sizeof(ies), &mct);
    n += AvdtpBuildMediaCodecCap(Out + n, Cap - n, mct, ies, l);
    if (All && Sep->DelayReporting && n + 2 <= Cap) { Out[n++] = AVDTP_CAT_DELAY_REPORTING; Out[n++] = 0; }
    return n;
}
static void TOldReject(TSess *S, UCHAR e) { UCHAR p[1] = { e }; S->Send(AVDTP_MSG_RESPONSE_REJECT, p, 1); }
static void TOldProcess(TSess *S, UCHAR SignalId, const UCHAR *Payload, ULONG Length)
{
    UCHAR rsp[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG n = 0;
    switch (SignalId) {
    case AVDTP_DISCOVER:
        for (ULONG i = 0; i < S->LocalSepCount && n + 2 <= sizeof(rsp); i++) {
            PAVDTP_LOCAL_SEP sep = &S->LocalSeps[i];
            BOOLEAN inUse = (S->StreamState != AvdtpStateIdle) && (S->LocalSeid == sep->Seid);
            rsp[n++] = (UCHAR)((sep->Seid << 2) | (inUse ? 0x02 : 0));
            rsp[n++] = (UCHAR)((sep->MediaType << 4) | (sep->Tsep << 3));
        }
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, rsp, n);
        break;
    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES: {
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        PAVDTP_LOCAL_SEP sep = TOldFindLocalSep(S, (UCHAR)(Payload[0] >> 2));
        if (!sep) { TOldReject(S, AVDTP_ERR_BAD_ACP_SEID); break; }
        n = TOldBuildLocalCapabilities(sep, SignalId == AVDTP_GET_ALL_CAPABILITIES, rsp, sizeof(rsp));
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, rsp, n);
        break;
    }
    case AVDTP_SET_CONFIGURATION: {
        if (Length < 2) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        UCHAR acpSeid = (UCHAR)(Payload[0] >> 2), intSeid = (UCHAR)(Payload[1] >> 2);
        PAVDTP_LOCAL_SEP sep = TOldFindLocalSep(S, acpSeid);
        if (!sep) { rsp[0] = 0; rsp[1] = AVDTP_ERR_BAD_ACP_SEID; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        if (S->StreamState != AvdtpStateIdle) { rsp[0] = 0; rsp[1] = AVDTP_ERR_SEP_IN_USE; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        AvdtpParseServiceCapabilities(Payload + 2, Length - 2, &tmp);
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC;
        UCHAR err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        if (tmp.CapsValid && tmp.Codec == sep->Codec)
            err = TAcp::Check(NULL, sep, intSeid, tmp.CodecCaps, tmp.CodecCapsLength, tmp.ServiceCaps, &failedCat);
        if (err) { rsp[0] = failedCat; rsp[1] = err; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        S->LocalSeid = acpSeid;
        S->RemoteSeid = intSeid;
        S->ConfiguredIesLength = tmp.CodecCapsLength;
        RtlCopyMemory(S->ConfiguredIes, tmp.CodecCaps, tmp.CodecCapsLength);
        S->ConfiguredServiceCaps = tmp.ServiceCaps;
        S->DelayReportingNegotiated = (tmp.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) != 0;
        S->SinkDelayTenthsMs = 0;   /* step 2 (T6): a new configuration resets the delay report */
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->SetState(AvdtpStateConfigured);
        break;
    }
    case AVDTP_GET_CONFIGURATION: {
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState == AvdtpStateIdle || (UCHAR)(Payload[0] >> 2) != S->LocalSeid) { TOldReject(S, AVDTP_ERR_BAD_ACP_SEID); break; }
        PAVDTP_LOCAL_SEP sep = TOldFindLocalSep(S, S->LocalSeid);
        UCHAR mct = A2DP_CODEC_SBC; UCHAR ies[32];
        A2dpBuildLocalCaps(sep ? sep->Codec : PNA2DP_CODEC_SBC, ies, sizeof(ies), &mct);
        rsp[n++] = AVDTP_CAT_MEDIA_TRANSPORT; rsp[n++] = 0;
        n += AvdtpBuildMediaCodecCap(rsp + n, sizeof(rsp) - n, mct, S->ConfiguredIes, S->ConfiguredIesLength);
        if (S->DelayReportingNegotiated) { rsp[n++] = AVDTP_CAT_DELAY_REPORTING; rsp[n++] = 0; }
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, rsp, n);
        break;
    }
    case AVDTP_RECONFIGURE: {
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen) { rsp[0] = 0; rsp[1] = AVDTP_ERR_BAD_STATE; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        AVDTP_REMOTE_SEP tmp;
        RtlZeroMemory(&tmp, sizeof(tmp));
        AvdtpParseServiceCapabilities(Payload + 1, Length - 1, &tmp);
        UCHAR failedCat = AVDTP_CAT_MEDIA_CODEC, err = AVDTP_ERR_UNSUPPORTED_CONFIGURATION;
        PAVDTP_LOCAL_SEP local = TOldFindLocalSep(S, S->LocalSeid);
        if (tmp.CapsValid && local && tmp.Codec == local->Codec)
            err = TAcp::CheckRe(NULL, tmp.CodecCaps, tmp.CodecCapsLength, &failedCat);
        if (err) { rsp[0] = failedCat; rsp[1] = err; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        S->ConfiguredIesLength = tmp.CodecCapsLength;
        RtlCopyMemory(S->ConfiguredIes, tmp.CodecCaps, tmp.CodecCapsLength);
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        break;
    }
    case AVDTP_OPEN:
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateConfigured) { TOldReject(S, AVDTP_ERR_BAD_STATE); break; }
        S->MediaExpected = TRUE;
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->SetState(AvdtpStateOpening);
        break;
    case AVDTP_START:
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen) { rsp[0] = Payload[0]; rsp[1] = AVDTP_ERR_BAD_STATE; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->SetState(AvdtpStateStreaming);
        break;
    case AVDTP_SUSPEND:
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateStreaming) { rsp[0] = Payload[0]; rsp[1] = AVDTP_ERR_BAD_STATE; S->Send(AVDTP_MSG_RESPONSE_REJECT, rsp, 2); break; }
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->SetState(AvdtpStateOpen);
        break;
    case AVDTP_CLOSE:
        if (Length < 1) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        if (S->StreamState != AvdtpStateOpen && S->StreamState != AvdtpStateStreaming) { TOldReject(S, AVDTP_ERR_BAD_STATE); break; }
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->SetState(AvdtpStateClosing);
        S->Disconnect();
        S->MediaUp = FALSE;
        S->SetState(AvdtpStateIdle);
        break;
    case AVDTP_ABORT:
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        if (S->StreamState != AvdtpStateIdle) {
            S->SetState(AvdtpStateAborting);
            S->Disconnect();
            S->MediaUp = FALSE;
            S->SetState(AvdtpStateIdle);
        }
        break;
    case AVDTP_DELAYREPORT: {
        if (Length < 3) { TOldReject(S, AVDTP_ERR_BAD_LENGTH); break; }
        ULONG delay = ((ULONG)Payload[1] << 8) | Payload[2];
        S->SinkDelayTenthsMs = delay;
        S->Send(AVDTP_MSG_RESPONSE_ACCEPT, NULL, 0);
        S->Delay(delay);
        break;
    }
    case AVDTP_SECURITY_CONTROL:
        TOldReject(S, AVDTP_ERR_NOT_SUPPORTED_COMMAND);
        break;
    default:
        S->Send(AVDTP_MSG_GENERAL_REJECT, NULL, 0);
        break;
    }
}
/* the new avdtp.cpp AvdtpProcessCommand: AvdtpAcpProcess + effects in the documented order */
static void TNewProcess(TSess *S, UCHAR SignalId, const UCHAR *Payload, ULONG Length)
{
    AVDTP_ACP_CONTEXT ctx;
    AVDTP_ACP_RESULT r;
    ctx.LocalSeps = S->LocalSeps; ctx.LocalSepCount = S->LocalSepCount; ctx.StreamState = S->StreamState;
    ctx.LocalSeid = S->LocalSeid; ctx.RemoteSeid = S->RemoteSeid; ctx.ConfiguredIes = S->ConfiguredIes;
    ctx.ConfiguredIesLength = S->ConfiguredIesLength; ctx.DelayReportingNegotiated = S->DelayReportingNegotiated;
    ctx.IntSignal = 0;   /* step 3: no command of ours outstanding, as in the old acceptor */
    ctx.CheckConfiguration = TAcp::Check; ctx.CheckReconfigure = TAcp::CheckRe; ctx.CheckContext = NULL;
    AvdtpAcpProcess(&ctx, SignalId, Payload, Length, &r);
    if (r.Effects & AVDTP_ACP_FX_STORE_CONFIG) {
        S->LocalSeid = r.ConfigLocalSeid; S->RemoteSeid = r.ConfigRemoteSeid; S->ConfiguredIesLength = r.IesLength;
        RtlCopyMemory(S->ConfiguredIes, r.Ies, r.IesLength);
        S->ConfiguredServiceCaps = r.ServiceCaps; S->DelayReportingNegotiated = r.DelayReporting;
    }
    if (r.Effects & AVDTP_ACP_FX_STORE_IES) { S->ConfiguredIesLength = r.IesLength; RtlCopyMemory(S->ConfiguredIes, r.Ies, r.IesLength); }
    if (r.Effects & AVDTP_ACP_FX_MEDIA_EXPECTED) S->MediaExpected = TRUE;
    if (r.Effects & AVDTP_ACP_FX_RESET_DELAY) S->SinkDelayTenthsMs = 0;
    if (r.Effects & AVDTP_ACP_FX_STORE_DELAY) S->SinkDelayTenthsMs = r.DelayTenthsMs;
    S->Send(r.MessageType, r.ResponseLength ? r.Response : NULL, r.ResponseLength);
    if (r.Effects & AVDTP_ACP_FX_MID_STATE) S->SetState(r.MidState);
    if (r.Effects & AVDTP_ACP_FX_DISCONNECT_MEDIA) { S->Disconnect(); S->MediaUp = FALSE; }
    if (r.Effects & AVDTP_ACP_FX_NEW_STATE) S->SetState(r.NewState);
    if (r.Effects & AVDTP_ACP_FX_DELAY_CALLBACK) S->Delay(r.DelayTenthsMs);
}

static void TestTDifferential()
{
    static const ULONG codecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC };
    static const UCHAR sbcFreqModes[] = { 0x3f, 0x2f, 0x1f, 0x11, 0x21, 0x12, 0x14, 0x38, 0xf1 };
    static const ULONG rates[] = { 44100, 48000, 96000, 88200, 32000, 16000 };
    AVDTP_LOCAL_SEP local[6];
    TBuildLocalSeps(local);
    unsigned chooseDiff = 0, chosen = 0, inUseFallback = 0, formatDiff = 0, formatActs[3] = {};
    for (unsigned it = 0; it < 30000; it++) {
        AVDTP_REMOTE_SEP remote[6];
        ULONG rc = TRnd() % 6;
        for (ULONG i = 0; i < rc; i++) {
            ULONG codec = codecs[TRnd() % 6];
            BOOLEAN inUse = (TRnd() % 5) == 0;
            remote[i] = (codec == PNA2DP_CODEC_SBC && (TRnd() & 1)) ? TSbcSinkSep((UCHAR)(i + 1), sbcFreqModes[TRnd() % 9], inUse)
                                                                    : TSinkSep((UCHAR)(i + 1), codec, inUse);
            if ((TRnd() % 9) == 0) remote[i].Tsep = AVDTP_TSEP_SOURCE;
            if ((TRnd() % 11) == 0) remote[i].CapsValid = FALSE;
        }
        PNA2DP_CONFIG cfg;
        memset(&cfg, 0, sizeof(cfg));
        for (ULONG k = 0, n = TRnd() % 5; k < n; k++) cfg.CodecPriority[k] = codecs[TRnd() % 6];
        cfg.EnabledCodecMask = TRnd() & 0x7e;
        cfg.Flags = TRnd() & (PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_AUTO_QUALITY);
        cfg.Sbc.MaxBitpool = (UCHAR)(TRnd() % 4 == 0 ? 0 : 20 + TRnd() % 80);
        ULONG host = TRnd() & 0x7e;
        BOOLEAN open = TRnd() & 1, conservative = (TRnd() % 4) == 0;
        ULONG rate = rates[TRnd() % 6], bits = (TRnd() & 1) ? 16 : 24;
        ULONG lc = (TRnd() % 7) == 0 ? (TRnd() % 6) : 6;

        A2DP_CODEC_CONFIG oldCfg, newCfg = {};
        UCHAR oldOwner = 0, oldLocal = 0;
        memset(&oldCfg, 0, sizeof(oldCfg));
        const AVDTP_REMOTE_SEP *r = TOldChooseConfig(remote, rc, local, lc, &cfg, host, open, rate, bits, conservative, &oldCfg, &oldOwner, &oldLocal);
        PN_CODEC_CHOICE_INPUT in;
        memset(&in, 0, sizeof(in));
        in.RemoteSeps = remote; in.RemoteSepCount = rc; in.LocalSeps = local; in.LocalSepCount = lc; in.Config = &cfg;
        in.HostCodecMask = host; in.RestrictRate = open; in.PreferredRate = open ? rate : 48000; in.PreferredBits = open ? bits : 16;
        in.Conservative = conservative;
        PN_CODEC_CHOICE c;
        memset(&c, 0, sizeof(c));
        BOOLEAN ok = PnChooseCodecConfig(&in, &c);
        newCfg = c.Cfg;
        BOOLEAN same;
        if (!r && ok) {
            /* step 2 (T1): where the old code found nothing, an in-use SEP is now tried last */
            same = remote[c.RemoteIndex].InUse != 0;
            inUseFallback++;
        } else {
            same = (r != NULL) == ok;
            if (same && ok) {
                same = c.RemoteIndex == (ULONG)(r - remote) && c.RemoteSeid == r->Seid && c.ServiceCaps == r->ServiceCaps &&
                       c.Owner == oldOwner && c.LocalSeid == oldLocal && memcmp(&newCfg, &oldCfg, sizeof(oldCfg)) == 0;
                chosen++;
            }
        }
        if (!same) chooseDiff++;

        /* stream format against the old Reconcile block */
        if (rc) {
            A2DP_CODEC_CONFIG active;
            A2DP_CODEC_CAPS caps;
            const AVDTP_REMOTE_SEP *cur = &remote[TRnd() % rc];
            PNA2DP_CONFIG pref = {};
            if (!cur->CapsValid || !A2dpParseCaps(cur->MediaCodecType, cur->CodecCaps, cur->CodecCapsLength, &caps, NULL, NULL)) continue;
            if (!A2dpSelectConfig(&caps, &pref, rates[TRnd() % 3], 16, &active)) continue;
            AVDTP_STREAM_STATE st = (AVDTP_STREAM_STATE)(TRnd() % 7);
            BOOLEAN activeValid = (TRnd() % 8) != 0;
            const AVDTP_REMOTE_SEP *remoteForStream = (TRnd() % 8) ? cur : NULL;
            A2DP_CODEC_CONFIG o1 = {}, o2 = {};
            int oldAct = TOldFormat(activeValid, open, st, &active, remoteForStream, &cfg, rate, bits, conservative, &o1);
            PN_FORMAT_INPUT f;
            memset(&f, 0, sizeof(f));
            f.ActiveValid = activeValid; f.Active = &active; f.Remote = remoteForStream; f.Config = &cfg; f.AudioStreamOpen = open;
            f.AudioRate = rate; f.AudioValidBits = bits; f.Conservative = conservative; f.StreamState = st;
            int newAct = (int)PnPlanStreamFormat(&f, &o2);
            if (oldAct != newAct || (oldAct == 1 && memcmp(&o1, &o2, sizeof(o1)) != 0)) formatDiff++;
            formatActs[oldAct]++;
        }
    }
    CHECK(chooseDiff == 0);
    CHECK(formatDiff == 0);
    CHECK(chosen > 5000 && formatActs[1] > 100 && formatActs[2] > 100 && inUseFallback > 50);   /* the inputs reach every branch */
    printf("lane T differential: choose %u/%u chosen (+%u in-use fallbacks), format keep/reconf/reselect %u/%u/%u\n", chosen, 30000u,
           inUseFallback, formatActs[0], formatActs[1], formatActs[2]);

    /* SDP: structured records with random versions, then random corruption */
    unsigned sdpDiff = 0;
    for (unsigned it = 0; it < 20000; it++) {
        UCHAR rec[256];
        ULONG n = TSdpRecord(rec, (USHORT)TRnd(), (USHORT)TRnd(), (USHORT)TRnd(), (TRnd() & 1) != 0);
        for (ULONG k = 0, m = TRnd() % 4; k < m; k++) rec[TRnd() % n] = (UCHAR)TRnd();
        ULONG len = (TRnd() % 4) ? n : TRnd() % (n + 1);
        TOldLink o = { 0xAAAA, 0xBBBB }, w = { 0xAAAA, 0xBBBB };
        BOOLEAN oldOk = TOldSdpParseRecord(&o, rec, len);
        PN_SDP_A2DP_INFO info;
        BOOLEAN newOk = PnSdpParseA2dpRecord(rec, len, &info);
        if (newOk) {
            /* step 2 (T5): the old parser stored the A2DP profile version as "AVDTP version";
             * that value is now the profile version, the AVDTP version comes from 0x0004 */
            if (info.ProfileVersionFound) w.RemoteAvdtpVersion = info.ProfileVersion;
            if (info.FeaturesSeen) w.RemoteSupportedFeatures = info.SupportedFeatures;
            if (PnSdpAvdtpVersionToUse(&info) != (info.AvdtpVersionFound ? info.AvdtpVersion : 0)) sdpDiff++;
        }
        if (oldOk != newOk || o.RemoteAvdtpVersion != w.RemoteAvdtpVersion || o.RemoteSupportedFeatures != w.RemoteSupportedFeatures) sdpDiff++;
    }
    CHECK(sdpDiff == 0);

    /* acceptor: random commands in random stream states */
    unsigned acpDiff = 0, acpCompared = 0, acpSkipped = 0;
    static const UCHAR signals[] = { AVDTP_DISCOVER, AVDTP_GET_CAPABILITIES, AVDTP_SET_CONFIGURATION, AVDTP_GET_CONFIGURATION, AVDTP_RECONFIGURE,
                                     AVDTP_OPEN, AVDTP_START, AVDTP_CLOSE, AVDTP_SUSPEND, AVDTP_ABORT, AVDTP_SECURITY_CONTROL,
                                     AVDTP_GET_ALL_CAPABILITIES, AVDTP_DELAYREPORT, 0x0E, 0x3F };
    for (unsigned it = 0; it < 40000; it++) {
        TSess a;
        memset(&a, 0, sizeof(a));
        TBuildLocalSeps(a.LocalSeps);
        a.LocalSepCount = (TRnd() % 9) == 0 ? TRnd() % 6 : 6;
        a.StreamState = (AVDTP_STREAM_STATE)(TRnd() % 7);
        a.LocalSeid = (UCHAR)(TRnd() % 8); a.RemoteSeid = (UCHAR)(TRnd() % 8);
        a.ConfiguredIesLength = (UCHAR)(TRnd() % 9);
        for (ULONG k = 0; k < a.ConfiguredIesLength; k++) a.ConfiguredIes[k] = (UCHAR)TRnd();
        a.DelayReportingNegotiated = TRnd() & 1;
        UCHAR sig = signals[TRnd() % PN_ARRAYSIZE(signals)];
        UCHAR p[64];
        ULONG plen;
        /* step 2 changed these by design (T6 DELAYREPORT checks, T7 strict categories of
         * SET_CONFIGURATION / RECONFIGURE): they are covered by direct tests instead */
        BOOLEAN changedByDesign = sig == AVDTP_DELAYREPORT || sig == AVDTP_SET_CONFIGURATION || sig == AVDTP_RECONFIGURE;
        if ((sig == AVDTP_SET_CONFIGURATION || sig == AVDTP_RECONFIGURE) && (TRnd() & 1)) {
            ULONG codec = codecs[TRnd() % 6];
            UCHAR ies[32], mct, caps[64];
            ULONG l = A2dpBuildLocalCaps(codec, ies, sizeof(ies), &mct);
            PNA2DP_CONFIG pref = {};
            A2DP_CODEC_CAPS cc; A2DP_CODEC_CONFIG one;
            if ((TRnd() & 3) && A2dpParseCaps(mct, ies, l, &cc, NULL, NULL) && A2dpSelectConfig(&cc, &pref, rates[TRnd() % 3], 16, &one)) { memcpy(ies, one.Ies, one.IesLength); l = one.IesLength; }
            BOOLEAN delay = TRnd() & 1;
            ULONG cn = TCapsBytes(caps, ies, l, mct, delay);
            /* well-formed: transport + codec (+ delay reporting, which RECONFIGURE may not change) */
            changedByDesign = sig == AVDTP_RECONFIGURE && delay;
            if (sig == AVDTP_SET_CONFIGURATION) plen = TSetConfig(p, (UCHAR)(TRnd() % 8), (UCHAR)(TRnd() % 64), caps, cn);
            else { p[0] = (UCHAR)((TRnd() % 8) << 2); memcpy(p + 1, caps + 2, cn - 2); plen = cn - 1; }
        } else {
            plen = TRnd() % 12;
            for (ULONG k = 0; k < plen; k++) p[k] = (UCHAR)TRnd();
            if (plen && (TRnd() & 1)) p[0] = (UCHAR)((TRnd() % 8) << 2);
        }
        TSess b = a;
        TAcp::checkError = (TRnd() % 5) == 0 ? A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE : 0;
        TOldProcess(&a, sig, p, plen);
        TNewProcess(&b, sig, p, plen);
        if (changedByDesign) { acpSkipped++; continue; }
        acpCompared++;
        if (memcmp(&a, &b, sizeof(a)) != 0) acpDiff++;
    }
    TAcp::checkError = 0;
    CHECK(acpDiff == 0 && acpCompared > 25000);
}

/* ---- RED: the corrected transport ---------------------------------------------------- */

static void TestTRedItems()
{
    PN_CODEC_CHOICE c;
    PN_RECONCILE_PLAN p;
    PN_RECONCILE_INPUT in;
    /* T1: our own stream's SEP is not "in use" for us; InUse means "try last" */
    {
        TChoiceEnv e;
        e.Add(TSinkSep(1, PNA2DP_CODEC_SBC, TRUE));
        e.in.OwnRemoteSeid = 1;
        RED("T1", e.Choose(&c) && c.RemoteSeid == 1);
        e.in.OwnRemoteSeid = 0;
        RED("T1", e.Choose(&c) && c.RemoteSeid == 1);   /* only an in-use SEP: still tried, last */
        e.Add(TSinkSep(2, PNA2DP_CODEC_SBC));
        CHECK(e.Choose(&c) && c.RemoteSeid == 2);       /* a free one first */
        TAcp a(AvdtpStateOpen, 1);
        a.Run(AVDTP_CLOSE, { 1 << 2 }); RED("T1", (a.r.Effects & AVDTP_ACP_FX_RELEASE_REMOTE) != 0);
        a.Run(AVDTP_ABORT, { 1 << 2 }); RED("T1", (a.r.Effects & AVDTP_ACP_FX_RELEASE_REMOTE) != 0);
        RED("T1", PnSetConfigFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_SEP_IN_USE, FALSE, FALSE) == PnSetConfigRediscover);
        RED("T1", PnSetConfigFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_SEP_IN_USE, FALSE, TRUE) == PnSetConfigTryNext);
    }
    /* T2: a receiver SUSPEND is respected */
    {
        TAcp a(AvdtpStateStreaming, 1);
        a.Run(AVDTP_SUSPEND, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateOpen);
        RED("T2", (a.r.Effects & AVDTP_ACP_FX_REMOTE_SUSPEND) != 0);
        a.ctx.StreamState = AvdtpStateOpen;
        a.Run(AVDTP_START, { 1 << 2 }); RED("T2", (a.r.Effects & AVDTP_ACP_FX_REMOTE_START) != 0);
        RED("T2", !PnPumpMayRun(AvdtpStateOpen));
        RED("T2", PnStatusState(AvdtpStateOpen, TRUE, TRUE) == PNA2DP_STATE_OPEN);
        in = TWant(AvdtpStateOpen); in.PumpStreaming = TRUE; in.RemoteSuspended = TRUE; in.RemoteSuspendAgeMs = 100;
        PnPlanReconcile(&in, &p);
        RED("T2", p.Count && p.Steps[0] == PnStepStopPump);
        RED("T2", !PnPlanHas(&p, PnStepStartIfOpen) && !PnPlanHas(&p, PnStepStartPump));
        RED("T2", p.RetryDelayMs >= 5000 && p.RetryDelayMs <= 10000);
        in.LocalStartRequested = TRUE; PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepStartIfOpen));          /* a new local start may START again */
        in = TWant(AvdtpStateStreaming); in.AudioRun = FALSE; in.RemoteStarted = TRUE;
        PnPlanReconcile(&in, &p);
        RED("T2", PnPlanHas(&p, PnStepArmSuspendTimer));
    }
    /* T3: a configured stream is aborted, a receiver-configured one waits for its OPEN */
    {
        RED("T3", PnStreamCloseAction(AvdtpStateConfigured) == PnCloseSendAbort);
        RED("T3", PnStreamCloseAction(AvdtpStateOpening) == PnCloseSendAbort);
        PNA2DP_CONFIG cfg = TConfig();
        AVDTP_REMOTE_SEP sbc = TSbcSinkSep(1, 0x3f);
        UCHAR offer44[] = { 0x21, 0x15, 2, 53 };
        A2DP_CODEC_CONFIG active, out;
        A2dpParseConfig(A2DP_CODEC_SBC, offer44, 4, &active);
        PN_FORMAT_INPUT f;
        memset(&f, 0, sizeof(f));
        f.ActiveValid = TRUE; f.Active = &active; f.Remote = &sbc; f.Config = &cfg; f.AudioStreamOpen = TRUE;
        f.AudioRate = 48000; f.AudioValidBits = 16; f.StreamState = AvdtpStateConfigured; f.RemoteConfigured = TRUE;
        RED("T3", PnPlanStreamFormat(&f, &out) == PnFormatWaitOpenReconfigure);
    }
    /* T4: every setup failure retries, with back-off, falling down the codec list */
    {
        RED("T4", TFail(PnStepEnsureDiscovered, PN_ST_NOT_SUPPORTED).RetryDelayMs > 0);
        RED("T4", TFail(PnStepEnsureOpen, PN_ST_CANCELLED).RetryDelayMs > 0);      /* media connect timeout */
        RED("T4", TFail(PnStepEnsureOpen, PN_ST_NOT_SUPPORTED).RetryDelayMs > 0);   /* no usable codec */
        RED("T4", TFail(PnStepStartIfOpen, PN_ST_DEVICE_PROTOCOL_ERROR).RetryDelayMs > 0);
        RED("T4", TFail(PnStepStartPump, PN_ST_DEVICE_NOT_READY).RetryDelayMs > 0);
        PN_SETUP_FAILURE f = TFail(PnStepEnsureOpen, PN_ST_IO_TIMEOUT, 0, 0);
        RED("T4", f.AbortStream && !f.Disconnect);                  /* first timeout: ABORT, keep signalling */
        f = TFail(PnStepEnsureOpen, PN_ST_IO_TIMEOUT, 1, 1);
        CHECK(f.Disconnect);                                         /* second consecutive timeout (also today) */
        RED("T4", TFail(PnStepEnsureOpen, PN_ST_CANCELLED, 4).RetryDelayMs == PnSetupRetryDelayMs(4));
        RED("T4", TFail(PnStepEnsureOpen, PN_ST_IO_TIMEOUT, 4, 0).RetryDelayMs >= 24000);   /* signalling up, OPEN times out */
        RED("T4", TFail(PnStepEnsureSignalling, PN_ST_IO_TIMEOUT, 4, 0, 0).RetryDelayMs >= PnSetupRetryDelayMs(4));
        RED("T4", PnSetConfigFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_UNSUPPORTED_CONFIGURATION, FALSE, FALSE) == PnSetConfigTryNext);
        TChoiceEnv e;
        e.Add(TSinkSep(1, PNA2DP_CODEC_SBC)); e.Add(TSinkSep(2, PNA2DP_CODEC_AAC)); e.Add(TSinkSep(3, PNA2DP_CODEC_LDAC));
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
        PnRejectedAdd(&e.rejected, PNA2DP_CODEC_LDAC, 3);
        RED("T4", e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_AAC);
        PnRejectedAdd(&e.rejected, PNA2DP_CODEC_AAC, 2);
        RED("T4", e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC);
    }
    /* C8: with a 2:1 decimating pump a 96 kHz engine feeds a 48 kHz-only sink */
    {
        TChoiceEnv e;
        e.Add(TSbcSinkSep(1, 0x1f));
        e.in.RestrictRate = TRUE; e.in.PreferredRate = 96000;
        CHECK(!e.Choose(&c));                                /* without decimation: no codec */
        e.in.RateConvert = PN_RATECONV_HALF;
        RED("C8", e.Choose(&c) && c.Cfg.SampleRate == 48000);
        e.in.PreferredRate = 88200; e.remote[0] = TSbcSinkSep(1, 0x2f);
        RED("C8", e.Choose(&c) && c.Cfg.SampleRate == 44100);
    }
    /* T5: discovery falls back per SEP and is valid only with capabilities; AVDTP version from SDP 0x0004 */
    {
        PN_CAPS_QUERY q;
        PnCapsQueryStart(&q, 0x0103, FALSE);
        RED("T5", PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsRetryBasic);
        PnCapsQueryStart(&q, 0x0103, FALSE);
        RED("T5", PnCapsQueryResult(&q, PN_ST_DEVICE_PROTOCOL_ERROR) == PnCapsRetryBasic);
        RED("T5", !PnDiscoveryValid(PN_ST_IO_TIMEOUT, 2, 0));
        RED("T5", !PnDiscoveryValid(0, 2, 0));
        CHECK(PnDiscoveryValid(0, 2, 1));
        UCHAR rec[256];
        PN_SDP_A2DP_INFO info;
        PnSdpParseA2dpRecord(rec, TSdpRecord(rec, 0x0102, 0x0104, 0), &info);
        RED("T5", PnSdpAvdtpVersionToUse(&info) == 0x0102);
        PnSdpParseA2dpRecord(rec, TSdpRecord(rec, 0x0103, 0x0102, 0), &info);
        RED("T5", PnSdpAvdtpVersionToUse(&info) == 0x0103);
        RED("T5", PnShouldQuerySdp(FALSE, 0, FALSE));     /* not known yet: query, whoever opened signalling */
        RED("T5", !PnShouldQuerySdp(TRUE, 0, TRUE));      /* answered once: never again for the device */
    }
    /* T6: the sink delay is the current stream's own report (the acceptor stores it only for
     * the stream's SEID and it is reset with every configuration), else a per-codec default.
     * A report is used even when delay reporting was not negotiated: the EDIFIER MR5 reports
     * 364 ms on its LDAC stream although its LDAC SEP does not offer the capability. */
    {
        RED("T6", PnSinkDelayUs(FALSE, TRUE, TRUE, 1500, PNA2DP_CODEC_SBC) == 0);
        CHECK(PnSinkDelayUs(TRUE, FALSE, TRUE, 5000, PNA2DP_CODEC_SBC) == 500000);
        CHECK(PnSinkDelayUs(TRUE, FALSE, TRUE, 3640, PNA2DP_CODEC_LDAC) == 364000);
        RED("T6", PnSinkDelayUs(TRUE, TRUE, FALSE, 0, PNA2DP_CODEC_AAC) == PnDefaultSinkDelayUs(PNA2DP_CODEC_AAC));
        CHECK(PnSinkDelayUs(TRUE, TRUE, TRUE, 1234, PNA2DP_CODEC_AAC) == 123400);
        UCHAR sbc44[] = { 0x21, 0x15, 2, 53 }, caps[40], pl[64];
        TAcp a;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, TRUE)));
        RED("T6", (a.r.Effects & AVDTP_ACP_FX_RESET_DELAY) != 0);
        TAcp b(AvdtpStateOpen, 1);
        b.Run(AVDTP_DELAYREPORT, { 2 << 2, 0x05, 0xDC });
        RED("T6", b.Rejected1(AVDTP_ERR_BAD_ACP_SEID) && !(b.r.Effects & AVDTP_ACP_FX_STORE_DELAY));
        TAcp i;
        i.Run(AVDTP_DELAYREPORT, { 1 << 2, 0x05, 0xDC });
        RED("T6", i.Rejected1(AVDTP_ERR_BAD_STATE));
    }
    /* T7: lenient capability parsing, strict configuration acceptance */
    {
        UCHAR twoCp[] = { 1,0, 4,2,0x02,0x00, 4,2,0x01,0x00, 7,6,0,0, 0x11,0x15,2,53 };
        AVDTP_REMOTE_SEP s = {};
        RED("T7", AvdtpParseServiceCapabilities(twoCp, sizeof(twoCp), &s) && s.Codec == PNA2DP_CODEC_SBC &&
                  s.CpTypeMask == (AVDTP_CP_MASK_SCMS_T | AVDTP_CP_MASK_DTCP));
        UCHAR unknown[] = { 1,0, 10,1,0x55, 7,6,0,0, 0x11,0x15,2,53 };
        RED("T7", AvdtpParseServiceCapabilities(unknown, sizeof(unknown), &s) && s.Codec == PNA2DP_CODEC_SBC);
        CHECK(s.UnknownCategory == 10);
        UCHAR cfgPayload[64];
        UCHAR withCp[] = { 1,0, 4,2,0x02,0x00, 7,6,0,0, 0x21,0x15,2,53 };
        TAcp a;
        a.Run(AVDTP_SET_CONFIGURATION, cfgPayload, TSetConfig(cfgPayload, 1, 5, withCp, sizeof(withCp)));
        RED("T7", a.Rejected(AVDTP_CAT_CONTENT_PROTECTION, A2DP_ERR_INVALID_CP_TYPE));
        UCHAR withRecovery[] = { 1,0, 3,3,1,2,2, 7,6,0,0, 0x21,0x15,2,53 };
        a.Run(AVDTP_SET_CONFIGURATION, cfgPayload, TSetConfig(cfgPayload, 1, 5, withRecovery, sizeof(withRecovery)));
        RED("T7", a.Rejected(AVDTP_CAT_RECOVERY, AVDTP_ERR_BAD_SERV_CATEGORY));
        UCHAR withUnknown[] = { 1,0, 10,1,0x55, 7,6,0,0, 0x21,0x15,2,53 };
        a.Run(AVDTP_SET_CONFIGURATION, cfgPayload, TSetConfig(cfgPayload, 1, 5, withUnknown, sizeof(withUnknown)));
        RED("T7", a.Rejected(10, AVDTP_ERR_BAD_SERV_CATEGORY));
        UCHAR noTransport[] = { 7,6,0,0, 0x21,0x15,2,53 };
        TAcp::checkCalls = 0;
        a.Run(AVDTP_SET_CONFIGURATION, cfgPayload, TSetConfig(cfgPayload, 1, 5, noTransport, sizeof(noTransport)));
        RED("T7", a.Rejected(AVDTP_CAT_MEDIA_TRANSPORT, AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT) && TAcp::checkCalls == 0);
    }
    /* T10: unavailable local SEPs are advertised in use; a receiver-chosen codec is upgraded once */
    {
        TAcp a;
        a.local[5].InUse = 1;   /* LDAC: no encoder */
        a.Run(AVDTP_DISCOVER, {});
        RED("T10", (a.r.Response[10] & 0x02) != 0);
        CHECK((a.r.Response[0] & 0x02) == 0);
        in = TWant(AvdtpStateConfigured); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 3000; in.BetterCodecAvailable = TRUE;
        PnPlanReconcile(&in, &p);
        RED("T10", PnPlanHas(&p, PnStepUpgradeCodec) && TPlanIndex(p, PnStepUpgradeCodec) < TPlanIndex(p, PnStepStartIfOpen));
        in.CodecUpgradeTried = TRUE; PnPlanReconcile(&in, &p);
        CHECK(!PnPlanHas(&p, PnStepUpgradeCodec));       /* once per session */
        in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.RemoteConfigured = TRUE; in.BetterCodecAvailable = TRUE;
        PnPlanReconcile(&in, &p);
        CHECK(!PnPlanHas(&p, PnStepUpgradeCodec));       /* not while playing: at the next pause */
    }
    /* T11: idle D3 in S0 suspends and keeps the session */
    {
        ULONG fx = PnPowerOnDx(TRUE);
        RED("T11", (fx & PN_PWR_SUSPEND_STREAM) && !(fx & PN_PWR_RESET_ON_RESUME));
        CHECK(PnPowerOnDx(FALSE) & PN_PWR_RESET_ON_RESUME);   /* real system sleep: full reset */
        RED("T11", !PnPowerResetOnResume(TRUE, TRUE, TRUE));
        CHECK(PnPowerResetOnResume(TRUE, FALSE, TRUE) && PnPowerResetOnResume(TRUE, TRUE, FALSE));
    }
    /* T12: media MTU fills whole baseband packets */
    {
        const ULONGLONG edr2 = 1ull << 25, edr3 = 1ull << 26;
        RED("T12", PnMediaMtuCap(1, 3 | edr2 | edr3) == 1017);
        RED("T12", PnMediaMtuCap(1, 3 | edr2) == 675);
        RED("T12", PnMediaMtuCap(1, 3) == 1013);            /* BR only: three DH5 */
        RED("T12", PnMediaMtuCap(0, 0) == 1017 && PnMediaMtuCap(1, 0) == 1017);   /* unknown */
        RED("T12", PnPreferredMediaMtu(1, 3 | edr2 | edr3) == 1017 && PnPreferredMediaMtu(1, 3 | edr2) == 675);
        RED("T12", PnEffectiveMediaMtu(1024, PnMediaMtuCap(1, 3 | edr2 | edr3), 672) == 1017);
        CHECK(PnEffectiveMediaMtu(895, PnMediaMtuCap(1, 3 | edr2 | edr3), 672) == 895);
    }
    /* T13: link loss and stalled media are noticed */
    {
        RED("T13", (PnAclEventAction(FALSE, TRUE) & PN_ACL_FX_LINK_LOST) != 0);
        in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.MediaStalled = TRUE;
        PnPlanReconcile(&in, &p);
        RED("T13", PnPlanHas(&p, PnStepAbortStream) && !PnPlanHas(&p, PnStepStartPump));
        /* review round 3 #6: the pump runs on while the step probes; the retry after a
         * teardown comes from the failure count the step leaves (T4), not this snapshot's */
        RED("R3-6", TPlanIs(p, { PnStepAbortStream }) && p.RetryDelayMs == 0);
        in.SetupFailures = 5; PnPlanReconcile(&in, &p);
        RED("R3-6", p.RetryDelayMs == 0);
        /* a 2-5 s fade: the probe is answered once the radio is back and the writes flow */
        RED("R3-6", PnStallProbeAction(0, FALSE, FALSE) == PnStallKeep);
        RED("R3-6", PnStallProbeAction(PN_ST_DEVICE_PROTOCOL_ERROR, FALSE, FALSE) == PnStallKeep);   /* a reject is an answer */
        CHECK(PnStallProbeAction(0, TRUE, FALSE) == PnStallAbort);                          /* signalling fine, media stuck */
        CHECK(PnStallProbeAction(PN_ST_IO_TIMEOUT, FALSE, FALSE) == PnStallDisconnect);     /* unreachable */
        CHECK(PnStallProbeAction(PN_ST_CONNECTION_DISCONNECTED, TRUE, FALSE) == PnStallDisconnect);
        CHECK(PnStallJackEvent(PN_ST_IO_TIMEOUT) == PnJackStallUnanswered);                 /* drops the session */
        in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.LinkLost = TRUE;
        PnPlanReconcile(&in, &p);
        RED("T13", PnPlanHas(&p, PnStepLinkLost) && PnPlanHas(&p, PnStepStopPump) && !PnPlanHas(&p, PnStepStartPump) && p.RetryDelayMs > 0);
    }
}

/* ---- RED: scripted receivers ------------------------------------------------------------ */

static void TestTRedScenarios()
{
    PN_RECONCILE_PLAN p;
    PN_RECONCILE_INPUT in;
    UCHAR caps[64], pl[64];
    UCHAR sbc44[] = { 0x21, 0x15, 2, 53 };
    /* Strict one-command-at-a-time sink: configures the stream itself, then sends
     * DELAYREPORT and waits for our answer before its OPEN; it never answers a command of
     * ours while one of its own is outstanding. */
    {
        TAcp a;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, TRUE)));
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateConfigured);
        a.ctx.StreamState = AvdtpStateConfigured; a.ctx.LocalSeid = 1; a.ctx.DelayReportingNegotiated = TRUE;
        a.Run(AVDTP_DELAYREPORT, { 1 << 2, 0x07, 0xD0 });
        CHECK(a.Accepted() && a.r.DelayTenthsMs == 2000);   /* answered without any INT state */
        in = TWant(AvdtpStateConfigured); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 100;
        PnPlanReconcile(&in, &p);
        RED("S-strict", PnPlanHas(&p, PnStepWaitRemoteOpen) && !PnPlanHas(&p, PnStepEnsureOpen));
        RED("S-strict", p.RetryDelayMs >= 500 && p.RetryDelayMs <= 2000);
        in.RemoteConfiguredAgeMs = 2500; PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepEnsureOpen));             /* its OPEN never came: ours */
        /* review #3: a collision only when our acceptor refused the receiver's OPEN meanwhile */
        RED("S-strict", PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, TRUE, 0) == PnOpenFailCollision);
        RED("S-strict", !PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, FALSE, AvdtpStateConfigured));
        a.Run(AVDTP_OPEN, { 1 << 2 });
        CHECK(a.Accepted() && (a.r.Effects & AVDTP_ACP_FX_MEDIA_EXPECTED));
        CHECK(PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, TRUE, AvdtpStateOpening));
        RED("S-strict", PnMediaConnectFailureAction(PN_ST_INVALID_DEVICE_STATE, TRUE, FALSE) == PnMediaConnectAdopt);
    }
    /* AVDTP 1.2 sink that ignores GET_ALL_CAPABILITIES (it times out) */
    {
        TDiscoverySink sink;
        sink.allStatus = PN_ST_IO_TIMEOUT;
        sink.count = 2;
        sink.seps[0] = TSinkSep(1, PNA2DP_CODEC_SBC);
        sink.seps[1] = TSinkSep(2, PNA2DP_CODEC_AAC);
        AVDTP_REMOTE_SEP got[4];
        ULONG withCaps = 0;
        BOOLEAN valid = TSimDiscover(sink, 0x0103, got, &withCaps);
        RED("S-avdtp12", withCaps == 2 && valid);
        TChoiceEnv e;
        e.Add(got[0]); e.Add(got[1]);
        PN_CODEC_CHOICE c;
        RED("S-avdtp12", e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC);
        sink.allStatus = PN_ST_NOT_SUPPORTED;                /* a sink that rejects it is handled today */
        valid = TSimDiscover(sink, 0x0103, got, &withCaps);
        CHECK(valid && withCaps == 2);
    }
    /* Multipoint sink that suspends us for another phone, then resumes */
    {
        TAcp a(AvdtpStateStreaming, 1);
        a.Run(AVDTP_SUSPEND, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateOpen);
        RED("S-multipoint", (a.r.Effects & AVDTP_ACP_FX_REMOTE_SUSPEND) != 0);
        in = TWant(AvdtpStateOpen); in.PumpStreaming = TRUE; in.RemoteSuspended = TRUE; in.RemoteSuspendAgeMs = 50;
        PnPlanReconcile(&in, &p);
        RED("S-multipoint", p.Count && p.Steps[0] == PnStepStopPump && !PnPlanHas(&p, PnStepStartIfOpen));
        RED("S-multipoint", PnStatusState(AvdtpStateOpen, TRUE, TRUE) != PNA2DP_STATE_STREAMING);
        a.ctx.StreamState = AvdtpStateOpen;
        a.Run(AVDTP_START, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateStreaming);
        RED("S-multipoint", (a.r.Effects & AVDTP_ACP_FX_REMOTE_START) != 0);
        in = TWant(AvdtpStateStreaming); in.RemoteStarted = TRUE;
        PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepStartPump));               /* playing again: the pump follows */
    }
    /* Sink that configures 44.1 kHz while the engine runs 48 kHz */
    {
        TAcp a;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, TCapsBytes(caps, sbc44, 4, A2DP_CODEC_SBC, FALSE)));
        CHECK(a.Accepted());
        in = TWant(AvdtpStateConfigured); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 200;
        PnPlanReconcile(&in, &p);
        RED("S-44k", PnPlanHas(&p, PnStepWaitRemoteOpen));
        PNA2DP_CONFIG cfg = TConfig();
        AVDTP_REMOTE_SEP sbc = TSbcSinkSep(5, 0x3f);
        A2DP_CODEC_CONFIG active, out;
        A2dpParseConfig(A2DP_CODEC_SBC, sbc44, 4, &active);
        PN_FORMAT_INPUT f;
        memset(&f, 0, sizeof(f));
        f.ActiveValid = TRUE; f.Active = &active; f.Remote = &sbc; f.Config = &cfg; f.AudioStreamOpen = TRUE;
        f.AudioRate = 48000; f.AudioValidBits = 16; f.StreamState = AvdtpStateConfigured; f.RemoteConfigured = TRUE;
        RED("S-44k", PnPlanStreamFormat(&f, &out) == PnFormatWaitOpenReconfigure);
        RED("S-44k", PnStreamCloseAction(AvdtpStateConfigured) != PnCloseLocalOnly);
        f.StreamState = AvdtpStateOpen;                      /* the receiver opened it: RECONFIGURE */
        CHECK(PnPlanStreamFormat(&f, &out) == PnFormatReconfigure && out.SampleRate == 48000);
        /* 0.1.0.22 review #1/#3: the same with the converter (PN_RATECONV_ANY, the shipped setting).
         * The stream has carried no audio, so a RECONFIGURE interrupts nothing: it goes to the
         * engine's own rate, with or without a channel-mode preference */
        f.RateConvert = PN_RATECONV_ANY;
        for (ULONG pref : { (ULONG)0, (ULONG)PNA2DP_CFG_DUAL_CHANNEL_PREFER }) {
            cfg.Flags = pref;
            f.StreamState = AvdtpStateConfigured;
            RED("R22-bridge", PnPlanStreamFormat(&f, &out) == PnFormatWaitOpenReconfigure && out.SampleRate == 48000);
            f.StreamState = AvdtpStateOpen;
            RED("R22-bridge", PnPlanStreamFormat(&f, &out) == PnFormatReconfigure && out.SampleRate == 48000 &&
                              out.ChannelMode == (ULONG)(pref ? PNA2DP_CHMODE_DUAL : PNA2DP_CHMODE_JOINT));
            f.StreamState = AvdtpStateStreaming;             /* the receiver STARTed it, our pump never ran */
            RED("R22-bridge", PnPlanStreamFormat(&f, &out) == PnFormatReconfigure && out.SampleRate == 48000);
        }
    }
    /* Sink that advertises LDAC and AAC but rejects our configuration of either */
    {
        TChoiceEnv e;
        e.Add(TSinkSep(1, PNA2DP_CODEC_SBC)); e.Add(TSinkSep(2, PNA2DP_CODEC_AAC)); e.Add(TSinkSep(3, PNA2DP_CODEC_LDAC));
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
        ULONG configured = PNA2DP_CODEC_NONE, attempts = 0;
        for (; attempts < 6 && configured == PNA2DP_CODEC_NONE; attempts++) {
            PN_CODEC_CHOICE c;
            if (!e.Choose(&c)) break;
            if (c.Cfg.Codec == PNA2DP_CODEC_SBC) { configured = c.Cfg.Codec; break; }
            PN_SETCONFIG_ACTION act = PnSetConfigFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE,
                                                               PnRequestDelayReporting(0, c.ServiceCaps, 0x0103), FALSE);
            if (act == PnSetConfigTryNext) { PnRejectedAdd(&e.rejected, c.Cfg.Codec, c.RemoteSeid); continue; }
            if (TFail(PnStepEnsureOpen, PN_ST_DEVICE_PROTOCOL_ERROR).RetryDelayMs == 0) break;   /* no retry: silence */
            PnRejectedAdd(&e.rejected, c.Cfg.Codec, c.RemoteSeid);
        }
        RED("S-reject", configured == PNA2DP_CODEC_SBC && attempts <= 3);
    }
    /* Sink listing two content-protection schemes */
    {
        UCHAR twoCp[] = { 1,0, 4,2,0x02,0x00, 4,2,0x01,0x00, 7,6,0,0, 0x3f,0xff,2,53 };
        TChoiceEnv e;
        e.Add(TSepFromCaps(1, FALSE, twoCp, sizeof(twoCp)));
        PN_CODEC_CHOICE c;
        RED("S-2cp", e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.RemoteSeid == 1);
    }
}

/* ---- regression: the hardware-validated path (Sony CMT-SBT40D, SBC only) ---------------
 * SBC 48 kHz dual channel at the saved preference, set up by us or by the receiver, must
 * come out of the corrected transport exactly as before (same codec, format, SEIDs and
 * the same plan), apart from the documented wait for a receiver's own OPEN. */
static void TestTSonySbcPath()
{
    PN_CODEC_CHOICE c;
    PN_RECONCILE_PLAN p;
    PN_RECONCILE_INPUT in;
    const std::initializer_list<int> setup = { PnStepEnsureSignalling, PnStepEnsureDiscovered, PnStepCheckFormat,
                                               PnStepEnsureOpen, PnStepStartIfOpen, PnStepStartPump };
    UCHAR sonyIes[4] = { 0x3f, 0xff, 2, 53 }, caps[40], pl[64];
    PNA2DP_CONFIG cfg = TConfig();               /* LDAC, AAC, SBC; no codec host */
    cfg.Flags = PNA2DP_CFG_DUAL_CHANNEL_PREFER;
    {   /* locally initiated */
        TChoiceEnv e;
        e.cfg = cfg;
        e.Add(TSepFromCaps(1, FALSE, caps, TCapsBytes(caps, sonyIes, 4, A2DP_CODEC_SBC, FALSE)));
        e.in.RestrictRate = TRUE; e.in.PreferredRate = 48000;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.Cfg.SampleRate == 48000 && c.Cfg.ChannelMode == PNA2DP_CHMODE_DUAL &&
              c.RemoteSeid == 1 && c.LocalSeid == 1 && c.Owner == PN_OWNER_KERNEL_SBC);
        CHECK(!PnRequestDelayReporting(cfg.Flags, c.ServiceCaps, 0x0103));
        CHECK(PnSinkDelayUs(TRUE, FALSE, FALSE, 0, PNA2DP_CODEC_SBC) == PN_SINK_DELAY_DEFAULT_US);
        in = TWant(AvdtpStateIdle); in.SignallingUp = FALSE;
        PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup) && p.RetryDelayMs == 0);
        in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE;
        PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup) && p.RetryDelayMs == 0);
        in.AudioRun = FALSE; PnPlanReconcile(&in, &p); CHECK(p.Count == 0 && p.RetryDelayMs == 0);
        in.SuspendDue = TRUE; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepStopPump, PnStepSuspend, PnStepNotifyState }));
        in = TWant(AvdtpStateOpen); in.LocalStartRequested = TRUE;   /* play after the suspend */
        PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
    }
    {   /* receiver-initiated: SET_CONFIGURATION 48 kHz dual channel, then its OPEN */
        UCHAR sbc48dual[4] = { 0x14, 0x15, 2, 53 };
        TAcp a;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 1, caps, TCapsBytes(caps, sbc48dual, 4, A2DP_CODEC_SBC, FALSE)));
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateConfigured && a.r.ConfigLocalSeid == 1 && a.r.ConfigRemoteSeid == 1 && !a.r.DelayReporting);
        in = TWant(AvdtpStateConfigured); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 10; in.BetterCodecAvailable = TRUE;
        PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, { PnStepWaitRemoteOpen }) && p.RetryDelayMs == PN_REMOTE_OPEN_WAIT_MS - 10);
        a.ctx.StreamState = AvdtpStateConfigured; a.ctx.LocalSeid = 1;
        a.Run(AVDTP_OPEN, { 1 << 2 }); CHECK(a.Accepted() && a.r.NewState == AvdtpStateOpening);
        /* Open: the format already matches; the upgrade check finds nothing better than SBC */
        A2DP_CODEC_CONFIG active, out;
        CHECK(A2dpParseConfig(A2DP_CODEC_SBC, sbc48dual, 4, &active));
        AVDTP_REMOTE_SEP sony = TSepFromCaps(1, TRUE, caps, TCapsBytes(caps, sonyIes, 4, A2DP_CODEC_SBC, FALSE));   /* DISCOVER: in use by our stream */
        PN_FORMAT_INPUT f;
        memset(&f, 0, sizeof(f));
        f.ActiveValid = TRUE; f.Active = &active; f.Remote = &sony; f.Config = &cfg; f.AudioStreamOpen = TRUE;
        f.AudioRate = 48000; f.AudioValidBits = 16; f.StreamState = AvdtpStateOpen; f.RemoteConfigured = TRUE;
        CHECK(PnPlanStreamFormat(&f, &out) == PnFormatKeep);
        TChoiceEnv e;
        e.cfg = cfg;
        e.Add(sony);
        e.in.RestrictRate = TRUE; e.in.OwnRemoteSeid = 1;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.RemoteSeid == 1);   /* T1: before the fix: no SEP at all */
        CHECK(PnCodecRank(&cfg, c.Cfg.Codec) >= PnCodecRank(&cfg, active.Codec));     /* no upgrade */
        in = TWant(AvdtpStateOpen); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 300; in.BetterCodecAvailable = TRUE;
        PnPlanReconcile(&in, &p);
        CHECK(TPlanIs(p, { PnStepEnsureSignalling, PnStepEnsureDiscovered, PnStepCheckFormat, PnStepUpgradeCodec,
                           PnStepEnsureOpen, PnStepStartIfOpen, PnStepStartPump }));
        in.CodecUpgradeTried = TRUE; PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
        in = TWant(AvdtpStateStreaming); in.PumpStreaming = TRUE; in.RemoteConfigured = TRUE; in.CodecUpgradeTried = TRUE;
        PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup) && p.RetryDelayMs == 0);
    }
    {   /* receiver-initiated at 44.1 kHz joint stereo under the 48 kHz engine (docs/06, 0.1.0.21 on
         * hardware: reconfigured to dual channel at 48 kHz, 672 kbps). With the shipped converter
         * setting the same native path (0.1.0.22 review #1/#3), not 44.1 kHz dual plus conversion */
        UCHAR sbc44joint[4] = { 0x21, 0x15, 2, 53 };
        A2DP_CODEC_CONFIG active, out;
        CHECK(A2dpParseConfig(A2DP_CODEC_SBC, sbc44joint, 4, &active));
        AVDTP_REMOTE_SEP sony = TSepFromCaps(1, TRUE, caps, TCapsBytes(caps, sonyIes, 4, A2DP_CODEC_SBC, FALSE));
        PN_FORMAT_INPUT f;
        memset(&f, 0, sizeof(f));
        f.ActiveValid = TRUE; f.Active = &active; f.Remote = &sony; f.Config = &cfg; f.AudioStreamOpen = TRUE;
        f.AudioRate = 48000; f.AudioValidBits = 16; f.StreamState = AvdtpStateConfigured; f.RemoteConfigured = TRUE;
        for (ULONG rc : { (ULONG)PN_RATECONV_HALF, (ULONG)PN_RATECONV_ANY }) {
            f.RateConvert = rc;
            f.StreamState = AvdtpStateConfigured;
            BOOLEAN wait = PnPlanStreamFormat(&f, &out) == PnFormatWaitOpenReconfigure;
            f.StreamState = AvdtpStateOpen;   /* its OPEN */
            BOOLEAN reconf = PnPlanStreamFormat(&f, &out) == PnFormatReconfigure;
            out.BitrateBps = A2dpNominalBitrate(&out);
            RED("R22-sony44", wait && reconf && out.SampleRate == 48000 && out.ChannelMode == PNA2DP_CHMODE_DUAL && out.SbcMaxBitpool == 53 &&
                              out.BitrateBps == 672000);
        }
    }
    /* setup back-off: a 30 s stream that ends also clears the failure count */
    PnSetupRetry r = {};
    PnSetupRetryFailed(&r); PnSetupRetryFailed(&r);
    PnSetupRetryObserve(&r, 1000, 1); PnSetupRetryObserve(&r, 31000, 0);
    CHECK(r.failures == 0);
    PnSetupRetryFailed(&r); PnSetupRetryObserve(&r, 40000, 1); PnSetupRetryObserve(&r, 50000, 0);
    CHECK(r.failures == 1);
    /* the plan arms no START while the receiver's suspend back-off runs, and START after it */
    in = TWant(AvdtpStateOpen); in.RemoteSuspended = TRUE; in.RemoteSuspendAgeMs = PN_REMOTE_SUSPEND_BACKOFF_MS;
    PnPlanReconcile(&in, &p); CHECK(TPlanIs(p, setup));
    /* review round 3 #3: one timed START per suspend episode. After it, a sink that refused
     * it or suspended again (a multipoint sink serving another phone) gets no further START
     * until playback starts here again or the sink STARTs; the pass only re-checks. */
    {
        PN_RECONCILE_INPUT rs = in;
        rs.RemoteSuspendRetried = TRUE; rs.RemoteSuspendAgeMs = 10 * PN_REMOTE_SUSPEND_BACKOFF_MS;
        PnPlanReconcile(&rs, &p);
        RED("R3-3", !PnPlanHas(&p, PnStepStartIfOpen) && p.RetryDelayMs == PN_REMOTE_SUSPEND_BACKOFF_MS);
        rs.LocalStartRequested = TRUE; PnPlanReconcile(&rs, &p);
        CHECK(TPlanIs(p, setup));                              /* a new local playback start */
        rs.LocalStartRequested = FALSE; rs.RemoteSuspended = FALSE; PnPlanReconcile(&rs, &p);
        CHECK(TPlanIs(p, setup));                              /* the receiver STARTed / no suspend */
        CHECK(PnRemoteSuspendKeepsRetried(TRUE, 0) && PnRemoteSuspendKeepsRetried(TRUE, PN_REMOTE_SUSPEND_EPISODE_MS - 1));
        CHECK(!PnRemoteSuspendKeepsRetried(TRUE, PN_REMOTE_SUSPEND_EPISODE_MS) && !PnRemoteSuspendKeepsRetried(FALSE, 0));
        /* the finding's timeline: the PC stream runs for 2 minutes, the sink suspends it at
         * 1 s for the phone and suspends every START of ours again at once */
        BOOLEAN suspended = TRUE, retried = FALSE;
        ULONG suspendAt = 1000, retryAt = 0, starts = 0;
        for (ULONG t = 1000; t <= 120000; t += 250) {
            rs = TWant(AvdtpStateOpen);
            rs.RemoteSuspended = suspended; rs.RemoteSuspendAgeMs = t - suspendAt; rs.RemoteSuspendRetried = retried;
            PnPlanReconcile(&rs, &p);
            if (!PnPlanHas(&p, PnStepStartIfOpen)) continue;
            starts++; retried = TRUE; retryAt = t;                       /* StartIfOpen: unprompted */
            if (!PnRemoteSuspendKeepsRetried(retried, t - retryAt)) retried = FALSE;   /* OnRemoteStreamControl */
            suspended = TRUE; suspendAt = t;
        }
        RED("R3-3", starts == 1);
        /* a spurious suspend long after the one START begins a new episode: one more try */
        CHECK(!PnRemoteSuspendKeepsRetried(TRUE, 5 * 60000));
    }
    CHECK(PnReconcileWantsAudio(&in));
    in.ScoActive = TRUE; in.ConfigFlags = PNA2DP_CFG_SUSPEND_ON_SCO; CHECK(!PnReconcileWantsAudio(&in));
    /* T7: RECONFIGURE may only change the codec */
    {
        TAcp a(AvdtpStateOpen, 1);
        UCHAR re[16]; ULONG n = 0;
        re[n++] = 9 << 2;
        n += AvdtpBuildMediaCodecCap(re + n, sizeof(re) - n, A2DP_CODEC_SBC, sonyIes, 4);
        re[n++] = AVDTP_CAT_DELAY_REPORTING; re[n++] = 0;
        a.Run(AVDTP_RECONFIGURE, re, n); CHECK(a.Rejected(AVDTP_CAT_DELAY_REPORTING, AVDTP_ERR_INVALID_CAPABILITIES));
    }
    /* T5: a downgrade is remembered: GET_CAPABILITIES from the start */
    {
        PN_CAPS_QUERY q;
        PnCapsQueryStart(&q, 0x0103, TRUE);
        CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES);
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_CONNECTION_DISCONNECTED) == PnCapsStop && q.UseAll);
    }
}

/* Step 3 (T8/T9): the acceptor runs while a command of ours is outstanding, so every
 * transaction's answer is judged against the stream as it is *now*, and the acceptor
 * judges the receiver's commands against our outstanding one. Scripted interleavings
 * of the pure pieces avdtp.cpp executes. */
static void TestTLocking()
{
    UCHAR caps[64], pl[64];
    UCHAR sbc48[] = { 0x11, 0x15, 2, 53 };
    ULONG capsLen = TCapsBytes(caps, sbc48, 4, A2DP_CODEC_SBC, FALSE);

    /* answers arriving after the receiver moved the stream */
    CHECK(PnIntOutcome(AVDTP_START, 0, AvdtpStateOpen, AvdtpStateOpen) == PnIntApply);
    RED("T9-lock", PnIntOutcome(AVDTP_START, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, AvdtpStateStreaming) == PnIntReached);
    RED("T9-lock", PnIntOutcome(AVDTP_START, 0, AvdtpStateOpen, AvdtpStateStreaming) == PnIntReached);
    RED("T9-lock", PnIntOutcome(AVDTP_START, 0, AvdtpStateOpen, AvdtpStateIdle) == PnIntLost);
    CHECK(PnIntOutcome(AVDTP_START, PN_ST_IO_TIMEOUT, AvdtpStateOpen, AvdtpStateOpen) == PnIntFailed);
    RED("T9-lock", PnIntOutcome(AVDTP_SUSPEND, PN_ST_IO_TIMEOUT, AvdtpStateStreaming, AvdtpStateOpen) == PnIntReached);
    CHECK(PnIntOutcome(AVDTP_SUSPEND, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateStreaming, AvdtpStateStreaming) == PnIntApply);
    CHECK(PnIntOutcome(AVDTP_SUSPEND, PN_ST_IO_TIMEOUT, AvdtpStateStreaming, AvdtpStateStreaming) == PnIntFailed);
    RED("T9-lock", PnIntOutcome(AVDTP_OPEN, 0, AvdtpStateConfigured, AvdtpStateIdle) == PnIntLost);
    RED("T9-lock", PnIntOutcome(AVDTP_OPEN, 0, AvdtpStateConfigured, AvdtpStateOpening) == PnIntPending);
    RED("T9-lock", PnIntOutcome(AVDTP_OPEN, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateConfigured, AvdtpStateOpen) == PnIntReached);
    CHECK(PnIntOutcome(AVDTP_OPEN, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateConfigured, AvdtpStateConfigured) == PnIntFailed);
    CHECK(PnIntOutcome(AVDTP_CLOSE, 0, AvdtpStateClosing, AvdtpStateClosing) == PnIntApply);
    CHECK(PnIntOutcome(AVDTP_ABORT, PN_ST_IO_TIMEOUT, AvdtpStateAborting, AvdtpStateAborting) == PnIntApply);
    RED("T9-lock", PnIntOutcome(AVDTP_CLOSE, 0, AvdtpStateClosing, AvdtpStateIdle) == PnIntReached);
    RED("T9-lock", PnIntOutcome(AVDTP_ABORT, 0, AvdtpStateAborting, AvdtpStateConfigured) == PnIntLost);
    CHECK(PnIntOutcome(AVDTP_SET_CONFIGURATION, 0, AvdtpStateIdle, AvdtpStateIdle) == PnIntApply);
    RED("T9-lock", PnIntOutcome(AVDTP_SET_CONFIGURATION, 0, AvdtpStateIdle, AvdtpStateConfigured) == PnIntLost);
    RED("T9-lock", PnIntOutcome(AVDTP_RECONFIGURE, 0, AvdtpStateOpen, AvdtpStateIdle) == PnIntLost);
    CHECK(PnIntOutcome(AVDTP_RECONFIGURE, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, AvdtpStateOpen) == PnIntFailed);

    /* transaction status: a wait ended without an answer (a new signalling session took
     * over) is a retryable cancellation, not a protocol error and not a lost channel */
    CHECK(PnTransactStatus(TRUE, TRUE, 0xFF, 0) == PN_ST_IO_TIMEOUT);
    CHECK(PnTransactStatus(FALSE, FALSE, AVDTP_MSG_RESPONSE_ACCEPT, 0) == PN_ST_CONNECTION_DISCONNECTED);
    CHECK(PnTransactStatus(FALSE, TRUE, AVDTP_MSG_RESPONSE_ACCEPT, 0) == 0);
    CHECK(PnTransactStatus(FALSE, TRUE, AVDTP_MSG_GENERAL_REJECT, 0) == PN_ST_NOT_SUPPORTED);
    CHECK(PnTransactStatus(FALSE, TRUE, AVDTP_MSG_RESPONSE_REJECT, AVDTP_ERR_NOT_SUPPORTED_COMMAND) == PN_ST_NOT_SUPPORTED);
    CHECK(PnTransactStatus(FALSE, TRUE, AVDTP_MSG_RESPONSE_REJECT, AVDTP_ERR_BAD_STATE) == PN_ST_DEVICE_PROTOCOL_ERROR);
    RED("T9-lock", PnTransactStatus(FALSE, TRUE, 0xFF, 0) == PN_ST_CANCELLED);

    /* ordering: a receiver command that followed the answer to ours waits until that
     * answer is applied (sequence numbers wrap) */
    CHECK(!PnAcpMustWait(5, 5) && !PnAcpMustWait(4, 5));
    RED("T9-lock", PnAcpMustWait(5, 4));
    RED("T9-lock", PnAcpMustWait(0, 0xFFFFFFFFu));
    CHECK(!PnAcpMustWait(0xFFFFFFFFu, 0));

    /* discovery interrupted by a new signalling session stops; it is not a downgrade */
    {
        PN_CAPS_QUERY q;
        PnCapsQueryStart(&q, 0x0103, FALSE);
        RED("T9-lock", PnCapsQueryResult(&q, PN_ST_CANCELLED) == PnCapsStop && q.UseAll && !q.Downgraded);
    }
    /* TGAVDP100 (GAVDP 1.3: 0.5 - 3 s) */
    {
        volatile ULONG tgavdp100 = AVDTP_RESPONSE_TIMEOUT_MS;
        RED("T9-lock", tgavdp100 >= 500 && tgavdp100 <= 3000);
    }

    /* S-dr: a sink that sends DELAYREPORT and waits for our answer before answering our
     * OPEN. Our OPEN is outstanding; the acceptor answers at once, then OPEN applies. */
    {
        TAcp a(AvdtpStateConfigured, 1);
        a.ctx.DelayReportingNegotiated = TRUE;
        a.ctx.IntSignal = AVDTP_OPEN;
        a.Run(AVDTP_DELAYREPORT, { 1 << 2, 0x05, 0xDC });
        CHECK(a.Accepted() && a.r.DelayTenthsMs == 1500);
        CHECK(PnIntOutcome(AVDTP_OPEN, 0, AvdtpStateConfigured, AvdtpStateConfigured) == PnIntApply);
        CHECK(PnOpenTransport(FALSE, FALSE, FALSE) == PnOpenConnect);
    }
    /* S-start: both sides START at once; the receiver's is accepted, ours comes back
     * BAD_STATE: a collision that reached the goal (no abort, pump may run) */
    {
        TAcp a(AvdtpStateOpen, 1);
        a.ctx.IntSignal = AVDTP_START;
        a.Run(AVDTP_START, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateStreaming && (a.r.Effects & AVDTP_ACP_FX_REMOTE_START));
        RED("T9-lock", PnIntOutcome(AVDTP_START, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, a.r.NewState) == PnIntReached);
        CHECK(PnPumpMayRun(a.r.NewState));
    }
    /* S-config: both sides SET_CONFIGURATION at once: the receiver's is refused (one stream
     * per device) with [0][SEP_IN_USE], without asking the device */
    {
        TAcp a;
        a.ctx.IntSignal = AVDTP_SET_CONFIGURATION;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, capsLen));
        RED("T9-lock", a.Rejected(0, AVDTP_ERR_SEP_IN_USE) && TAcp::checkCalls == 0);
        a.ctx.IntSignal = 0;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, capsLen));
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateConfigured);
    }
    /* S-open: both sides OPEN at once: exactly one OPEN wins, so exactly one side connects
     * the transport. The receiver's is refused while ours is outstanding. */
    {
        TAcp a(AvdtpStateConfigured, 1);
        a.ctx.IntSignal = AVDTP_OPEN;
        a.Run(AVDTP_OPEN, { 1 << 2 });
        RED("T9-lock", a.Rejected1(AVDTP_ERR_BAD_STATE) && !(a.r.Effects & (AVDTP_ACP_FX_MEDIA_EXPECTED | AVDTP_ACP_FX_NEW_STATE)));
        a.ctx.IntSignal = 0;
        a.Run(AVDTP_OPEN, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateOpening);
    }
    /* S-media: the sink opens the media channel itself right after accepting our OPEN, and
     * its channel completes before we processed the answer: accepted (OPEN sent), held,
     * then adopted by AvdtpOpen without a second connect */
    {
        CHECK(PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, TRUE, AvdtpStateConfigured));
        RED("T9-lock", PnMediaUpAction(TRUE, AvdtpStateConfigured, TRUE) == PnMediaUpHold);
        CHECK(PnIntOutcome(AVDTP_OPEN, 0, AvdtpStateConfigured, AvdtpStateConfigured) == PnIntApply);
        RED("T9-lock", PnOpenTransport(TRUE, TRUE, TRUE) == PnOpenAdoptReady);
        CHECK(PnOpenTransport(TRUE, TRUE, FALSE) == PnOpenAdoptPending);
        /* review round 3 #2: a MediaUp left over from a channel the receiver closed right
         * after it came up is no transport to adopt: connect it ourselves */
        RED("R3-2", PnOpenTransport(TRUE, FALSE, FALSE) == PnOpenConnect);
        RED("R3-2", !PnRemoteMediaCurrent(TRUE, TRUE, FALSE));   /* closed / closing */
        RED("R3-2", !PnRemoteMediaCurrent(TRUE, FALSE, TRUE));   /* a newer connection */
        RED("R3-2", !PnRemoteMediaCurrent(FALSE, TRUE, TRUE));   /* our own connect */
        CHECK(PnRemoteMediaCurrent(TRUE, TRUE, TRUE));
        /* our connect succeeded, then the receiver disconnected before AvdtpOpen looked */
        RED("R3-2", !PnOwnMediaLive(FALSE, FALSE));
        CHECK(PnOwnMediaLive(TRUE, FALSE) && !PnOwnMediaLive(TRUE, TRUE));
        /* that failed connect aborts instead of adopting the stale flag */
        RED("R3-2", PnMediaConnectFailureAction(PN_ST_CONNECTION_DISCONNECTED, TRUE && PnRemoteMediaCurrent(TRUE, TRUE, FALSE), FALSE) == PnMediaConnectAbort);
        /* the usual order: our answer first, then its channel */
        CHECK(PnMediaUpAction(TRUE, AvdtpStateOpening, FALSE) == PnMediaUpOpenStream);
        /* no OPEN anywhere: not ours */
        CHECK(PnMediaUpAction(TRUE, AvdtpStateConfigured, FALSE) == PnMediaUpDrop);
        CHECK(PnMediaUpAction(FALSE, AvdtpStateConfigured, TRUE) == PnMediaUpDrop);
        CHECK(!PnAcceptIncomingAsMedia(TRUE, FALSE, FALSE, FALSE, AvdtpStateConfigured));
    }
    /* S-close: the receiver ABORTs while our CLOSE is outstanding, then configures again
     * before our late answer: the answer must not tear down its new stream */
    {
        TAcp a(AvdtpStateClosing, 1);
        a.ctx.IntSignal = AVDTP_CLOSE;
        a.Run(AVDTP_ABORT, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateIdle && (a.r.Effects & AVDTP_ACP_FX_DISCONNECT_MEDIA));
        RED("T9-lock", PnIntOutcome(AVDTP_CLOSE, 0, AvdtpStateClosing, AvdtpStateIdle) == PnIntReached);
        a.ctx.StreamState = AvdtpStateIdle; a.ctx.IntSignal = 0;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, capsLen));
        CHECK(a.Accepted());
        RED("T9-lock", PnIntOutcome(AVDTP_CLOSE, 0, AvdtpStateClosing, AvdtpStateConfigured) == PnIntLost);
    }
    /* S-order: the sink accepts our SET_CONFIGURATION and sends its OPEN right behind the
     * answer. The OPEN must see Configured, i.e. wait until our answer is applied. */
    {
        ULONG responseSeq = 7, applied = 6;   /* answer 7 received, not yet applied */
        ULONG openAfter = responseSeq;
        RED("T9-lock", PnAcpMustWait(openAfter, applied));
        applied = 7;
        CHECK(!PnAcpMustWait(openAfter, applied));
        TAcp a(AvdtpStateConfigured, 1);
        a.Run(AVDTP_OPEN, { 1 << 2 });
        CHECK(a.Accepted());
    }
}

/* Review round 1 (findings #1-#5): the transport fixes after the integrated review. */
static void TestTReview1()
{
    /* #1: a new signalling session accepted while a stream of the old one exists resets
     * that stream first; until then no command of the old session's procedures goes out
     * (it could not be answered: the reset waits for the command lock) and a media connect
     * the reset cancelled is neither aborted nor adopted. */
    CHECK(PnTransactPrecheck(TRUE, FALSE) == 0);
    CHECK(PnTransactPrecheck(FALSE, FALSE) == PN_ST_CONNECTION_DISCONNECTED);
    CHECK(PnTransactPrecheck(FALSE, TRUE) == PN_ST_CONNECTION_DISCONNECTED);
    CHECK(PnTransactPrecheck(TRUE, TRUE) == PN_ST_CANCELLED);
    CHECK(PnMediaConnectFailureAction(PN_ST_CANCELLED, FALSE, TRUE) == PnMediaConnectReset);
    CHECK(PnMediaConnectFailureAction(PN_ST_CANCELLED, TRUE, TRUE) == PnMediaConnectReset);
    CHECK(PnMediaConnectFailureAction(PN_ST_IO_TIMEOUT, FALSE, FALSE) == PnMediaConnectAbort);
    CHECK(PnMediaConnectFailureAction(PN_ST_INVALID_DEVICE_STATE, TRUE, FALSE) == PnMediaConnectAdopt);

    /* #2: the ABORT after an unanswered SET_CONFIGURATION keeps IntSignal at
     * SET_CONFIGURATION, so a receiver SET_CONFIGURATION is refused until it is answered */
    {
        UCHAR sbc[] = { 0x21, 0x15, 2, 53 }, caps[40], pl[64];
        TAcp a;
        a.ctx.IntSignal = AVDTP_SET_CONFIGURATION;
        a.Run(AVDTP_SET_CONFIGURATION, pl, TSetConfig(pl, 1, 5, caps, TCapsBytes(caps, sbc, 4, A2DP_CODEC_SBC, FALSE)));
        CHECK(a.Rejected(0, AVDTP_ERR_SEP_IN_USE) && !(a.r.Effects & AVDTP_ACP_FX_STORE_CONFIG));
    }

    /* #3: BAD_STATE on OPEN is a collision only when our acceptor refused the receiver's
     * OPEN while ours was outstanding, and only once in a row; otherwise (a BlueZ or
     * Fluoride sink whose SEP went Idle after a lost transport) ABORT and configure again */
    {
        TAcp a(AvdtpStateConfigured, 1);
        a.ctx.IntSignal = AVDTP_OPEN;
        a.Run(AVDTP_OPEN, { 9 << 2 });
        CHECK(a.r.MessageType == AVDTP_MSG_RESPONSE_REJECT && (a.r.Effects & AVDTP_ACP_FX_OPEN_COLLISION));
        TAcp idle;
        idle.ctx.IntSignal = AVDTP_OPEN;
        idle.Run(AVDTP_OPEN, { 9 << 2 });
        CHECK(idle.r.MessageType == AVDTP_MSG_RESPONSE_REJECT && !(idle.r.Effects & AVDTP_ACP_FX_OPEN_COLLISION));
        TAcp plain(AvdtpStateConfigured, 1);
        plain.Run(AVDTP_OPEN, { 1 << 2 });
        CHECK(plain.Accepted() && !(plain.r.Effects & AVDTP_ACP_FX_OPEN_COLLISION));
    }
    RED("R1-3", PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, FALSE, 0) == PnOpenFailAbort);
    CHECK(PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, TRUE, 0) == PnOpenFailCollision);
    RED("R1-3", PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, TRUE, 1) == PnOpenFailAbort);
    CHECK(PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_ACP_SEID, TRUE, 0) == PnOpenFailAbort);
    CHECK(PnOpenFailureAction(PN_ST_IO_TIMEOUT, 0, TRUE, 0) == PnOpenFailKeep);
    {
        /* the media-only loss loop: our stream Configured, the sink Idle. Each pass sends
         * OPEN and gets BAD_STATE; it must ABORT at once (no receiver OPEN was refused) */
        ULONG collisions = 0, opens = 0, aborts = 0;
        for (ULONG pass = 0; pass < 5 && !aborts; pass++) {
            opens++;
            PN_OPEN_ACTION act = PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, FALSE, collisions);
            collisions = act == PnOpenFailCollision ? collisions + 1 : 0;
            if (act == PnOpenFailAbort) aborts++;
        }
        RED("R1-3", opens == 1 && aborts == 1);
        /* a real collision every time: one retry, then ABORT */
        collisions = 0; opens = 0; aborts = 0;
        for (ULONG pass = 0; pass < 5 && !aborts; pass++) {
            opens++;
            PN_OPEN_ACTION act = PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, TRUE, collisions);
            collisions = act == PnOpenFailCollision ? collisions + 1 : 0;
            if (act == PnOpenFailAbort) aborts++;
        }
        RED("R1-3", opens == 2 && aborts == 1);
    }

    /* #4: GET_ALL_CAPABILITIES failing is a downgrade only on evidence: refused as a
     * command, or the basic query of the same SEP answering. A dead link or a per-SEP
     * reject on both queries leaves the version and GET_ALL_CAPABILITIES alone. */
    {
        PN_CAPS_QUERY q;
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsRetryBasic && PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES);
        CHECK(PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsStop);
        RED("R1-4", !q.Downgraded && q.Version == 0x0103);
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsRetryBasic);
        CHECK(PnCapsQueryResult(&q, PN_ST_CONNECTION_DISCONNECTED) == PnCapsStop);
        RED("R1-4", !q.Downgraded && q.Version == 0x0103);
        /* SEP 1 rejects both (BAD_ACP_SEID), SEP 2 answers GET_ALL_CAPABILITIES */
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_DEVICE_PROTOCOL_ERROR) == PnCapsRetryBasic);
        CHECK(PnCapsQueryResult(&q, PN_ST_DEVICE_PROTOCOL_ERROR) == PnCapsNextSep);
        RED("R1-4", PnCapsQuerySignal(&q) == AVDTP_GET_ALL_CAPABILITIES);
        CHECK(PnCapsQueryResult(&q, 0) == PnCapsParse);
        RED("R1-4", !q.Downgraded && q.Version == 0x0103);
        /* a 1.2 sink that ignores GET_ALL_CAPABILITIES: the basic answer is the evidence */
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_IO_TIMEOUT) == PnCapsRetryBasic);
        CHECK(PnCapsQueryResult(&q, 0) == PnCapsParse && q.Downgraded && q.Version == 0x0102);
        CHECK(PnCapsQuerySignal(&q) == AVDTP_GET_CAPABILITIES);
        /* refused as a command: proof at once */
        PnCapsQueryStart(&q, 0x0103, FALSE);
        CHECK(PnCapsQueryResult(&q, PN_ST_NOT_SUPPORTED) == PnCapsRetryBasic && q.Downgraded && q.Version == 0x0102);
    }

    /* #5: the SDP query is retried after a failure: once per signalling session, at most
     * PN_SDP_MAX_ATTEMPTS times per device; never again after one succeeded */
    CHECK(PnShouldQuerySdp(FALSE, 0, FALSE));
    CHECK(PnShouldQuerySdp(FALSE, 1, FALSE));
    RED("R1-5", !PnShouldQuerySdp(FALSE, 1, TRUE));
    RED("R1-5", !PnShouldQuerySdp(FALSE, PN_SDP_MAX_ATTEMPTS, FALSE));
    CHECK(!PnShouldQuerySdp(TRUE, 0, FALSE));
}

/* Review round 2 (transport findings #1/#3 and #2). */
static void TestTReview2()
{
    /* #1/#3: the receiver's OPEN crosses ours, so its command arrives before its BAD_STATE
     * answer to ours (one reader: arrival order). Our procedure judges the answer only once
     * the acceptor finished the commands queued before it, whichever thread the scheduler
     * runs first; the acceptor then still sees IntSignal OPEN and refuses the receiver's
     * OPEN as the collision, so our OPEN is retried instead of the stream aborted. */
    CHECK(!PnIntMustWaitForAcp(5, 5) && !PnIntMustWaitForAcp(4, 5));
    CHECK(PnIntMustWaitForAcp(0, 0xFFFFFFFFu) && !PnIntMustWaitForAcp(0xFFFFFFFFu, 0));
    {
        ULONG queued = 0, done = 0, responseSeq = 6, applied = 6;
        queued++;                                     /* receiver OPEN: AfterResponse 6 */
        ULONG openAfter = responseSeq;
        responseSeq++;                                /* the reject of our OPEN */
        ULONG before = queued;                        /* Txn.AcpBefore */
        CHECK(!PnAcpMustWait(openAfter, applied));    /* the command does not wait for us */
        RED_R2("R2-1", PnIntMustWaitForAcp(before, done));   /* we wait for it instead */
        /* what the wait prevents: judged first, IntSignal 0, the receiver's OPEN accepted
         * and our BAD_STATE taken for a SEP that went Idle: ABORT of its stream */
        {
            TAcp late(AvdtpStateConfigured, 1);
            late.Run(AVDTP_OPEN, { 1 << 2 });
            CHECK(late.Accepted() && late.r.NewState == AvdtpStateOpening);
            CHECK(PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, FALSE, 0) == PnOpenFailAbort);
        }
        TAcp a(AvdtpStateConfigured, 1);
        a.ctx.IntSignal = AVDTP_OPEN;                 /* AvdtpTxnEnd has not run */
        a.Run(AVDTP_OPEN, { 1 << 2 });
        BOOLEAN collided = (a.r.Effects & AVDTP_ACP_FX_OPEN_COLLISION) != 0;
        done++;
        CHECK(!PnIntMustWaitForAcp(before, done));
        CHECK(collided && a.Rejected1(AVDTP_ERR_BAD_STATE) && !(a.r.Effects & AVDTP_ACP_FX_MEDIA_EXPECTED));
        CHECK(PnIntOutcome(AVDTP_OPEN, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateConfigured, AvdtpStateConfigured) == PnIntFailed);
        RED_R2("R2-3", PnOpenFailureAction(PN_ST_DEVICE_PROTOCOL_ERROR, AVDTP_ERR_BAD_STATE, collided, 0) == PnOpenFailCollision);
        /* a command that arrives after the answer is not waited for (it waits for us) */
        queued++;
        CHECK(!PnIntMustWaitForAcp(before, done) && PnAcpMustWait(responseSeq, applied));
    }
    /* the same for START: the receiver's START applied first, our BAD_STATE is the goal reached */
    {
        ULONG queued = 1, done = 0, before = queued;
        RED_R2("R2-1", PnIntMustWaitForAcp(before, done));
        TAcp a(AvdtpStateOpen, 1);
        a.ctx.IntSignal = AVDTP_START;
        a.Run(AVDTP_START, { 1 << 2 });
        done++;
        CHECK(!PnIntMustWaitForAcp(before, done));
        RED_R2("R2-1", PnIntOutcome(AVDTP_START, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, a.r.NewState) == PnIntReached);
        /* judged before it (the lost race): a failure and an error event */
        CHECK(PnIntOutcome(AVDTP_START, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, AvdtpStateOpen) == PnIntFailed);
    }

    /* #2: paused with the stream open, the receiver configured it and we have not
     * discovered its SEPs this session: the one upgrade try is not spent on an empty list
     * (no discovery while paused); the play pass discovers, then upgrades */
    {
        PN_RECONCILE_PLAN p;
        PN_RECONCILE_INPUT in = TWant(AvdtpStateOpen);
        in.AudioRun = FALSE; in.RemoteConfigured = TRUE; in.BetterCodecAvailable = TRUE; in.RemoteSepsUnknown = TRUE;
        PnPlanReconcile(&in, &p);
        RED_R2("R2-2", !PnPlanHas(&p, PnStepUpgradeCodec));
        CHECK(!PnPlanHas(&p, PnStepEnsureDiscovered) && !PnPlanHas(&p, PnStepEnsureSignalling));
        in.RemoteSepsUnknown = FALSE;                 /* SEPs known: at the pause, as before */
        PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepUpgradeCodec));
        /* streaming and suspended at the pause: the same */
        in = TWant(AvdtpStateStreaming);
        in.AudioRun = FALSE; in.SuspendDue = TRUE; in.PumpStreaming = TRUE;
        in.RemoteConfigured = TRUE; in.BetterCodecAvailable = TRUE; in.RemoteSepsUnknown = TRUE;
        PnPlanReconcile(&in, &p);
        RED_R2("R2-2", PnPlanHas(&p, PnStepSuspend) && !PnPlanHas(&p, PnStepUpgradeCodec));
        /* the try was kept, so playing discovers first and then upgrades */
        in = TWant(AvdtpStateOpen);
        in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 3000; in.BetterCodecAvailable = TRUE; in.RemoteSepsUnknown = TRUE;
        PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepUpgradeCodec) && TPlanIndex(p, PnStepEnsureDiscovered) < TPlanIndex(p, PnStepUpgradeCodec) &&
              TPlanIndex(p, PnStepUpgradeCodec) < TPlanIndex(p, PnStepStartIfOpen));
    }
}

/* Review round 4. #1: only an accepted signalling channel is the receiver connecting; a
 * rejected channel (collision, a second one, media without OPEN) or a media accept must not
 * lift a pending user disconnect (C7). #2: our OPEN's media connect and the media disconnect
 * after our CLOSE / ABORT run after their transactions ended (T9); a receiver command whose
 * answer depends on the stream state waits for them, as HEAD's command lock made it.
 * #7: audio resuming inside an endpoint that stayed in RUN is a local playback start. */
static void TestTReview4()
{
    /* #1 */
    CHECK(PnIncomingKind(FALSE, FALSE, FALSE, FALSE, FALSE, AvdtpStateIdle) == PnIncomingSignalling);
    RED_R2("R4-1", PnIncomingKind(FALSE, TRUE, FALSE, FALSE, FALSE, AvdtpStateIdle) == PnIncomingReject);   /* collision */
    RED_R2("R4-1", PnIncomingKind(TRUE, FALSE, TRUE, FALSE, FALSE, AvdtpStateStreaming) == PnIncomingReject); /* a second channel */
    RED_R2("R4-1", PnIncomingKind(TRUE, FALSE, FALSE, FALSE, FALSE, AvdtpStateConfigured) == PnIncomingReject); /* media without OPEN */
    CHECK(PnIncomingKind(TRUE, FALSE, FALSE, FALSE, TRUE, AvdtpStateConfigured) == PnIncomingMedia);
    CHECK(PnIncomingKind(TRUE, FALSE, FALSE, FALSE, FALSE, AvdtpStateOpening) == PnIncomingMedia);
    CHECK(PnIncomingKind(TRUE, FALSE, FALSE, TRUE, TRUE, AvdtpStateOpening) == PnIncomingReject);         /* our connect runs */
    /* the same decision as the media acceptance it replaces */
    for (int m = 0; m < 32; ++m) for (int st = AvdtpStateIdle; st <= AvdtpStateAborting; ++st) {
        BOOLEAN sig = (m & 1) != 0, mu = (m & 2) != 0, mc = (m & 4) != 0, ex = (m & 8) != 0, sc = (m & 16) != 0;
        PN_INCOMING_KIND k = PnIncomingKind(sig, sc, mu, mc, ex, (AVDTP_STREAM_STATE)st);
        if (!sig && !sc) CHECK(k == PnIncomingSignalling);
        else CHECK((k == PnIncomingMedia) == PnAcceptIncomingAsMedia(sig, mu, mc, ex, (AVDTP_STREAM_STATE)st));
    }
    RED_R2("R4-1", !PnIncomingLiftsUserDisconnect(PnIncomingReject, TRUE) && !PnIncomingLiftsUserDisconnect(PnIncomingMedia, TRUE));
    CHECK(PnIncomingLiftsUserDisconnect(PnIncomingSignalling, TRUE) && !PnIncomingLiftsUserDisconnect(PnIncomingSignalling, FALSE));

    /* #2 */
    RED_R2("R4-2", PnAcpWaitsForTransport(AVDTP_START, TRUE) && PnAcpWaitsForTransport(AVDTP_SET_CONFIGURATION, TRUE));
    CHECK(PnAcpWaitsForTransport(AVDTP_CLOSE, TRUE) && PnAcpWaitsForTransport(AVDTP_DISCOVER, TRUE) && PnAcpWaitsForTransport(AVDTP_OPEN, TRUE));
    CHECK(!PnAcpWaitsForTransport(AVDTP_START, FALSE) && !PnAcpWaitsForTransport(AVDTP_SET_CONFIGURATION, FALSE));
    /* the receiver's escape (ABORT) and state-free commands are answered at once */
    CHECK(!PnAcpWaitsForTransport(AVDTP_ABORT, TRUE) && !PnAcpWaitsForTransport(AVDTP_DELAYREPORT, TRUE) &&
          !PnAcpWaitsForTransport(AVDTP_GET_CAPABILITIES, TRUE));
    {
        TAcp early(AvdtpStateOpening, 1);
        early.Run(AVDTP_START, { 1 << 2 });
        CHECK(!early.Accepted());                     /* judged inside the window */
        TAcp late(AvdtpStateOpen, 1);
        late.Run(AVDTP_START, { 1 << 2 });
        CHECK(late.Accepted());                       /* judged after the wait */
    }

    /* #7 */
    {
        UCHAR pcm[64] = {};
        CHECK(!PnPcmHasSignal(pcm, sizeof(pcm)) && !PnPcmHasSignal(pcm, 0));
        pcm[63] = 1;
        CHECK(PnPcmHasSignal(pcm, sizeof(pcm)) && !PnPcmHasSignal(pcm, 63));
        PN_RESUME_DETECT r = {};
        unsigned edges = 0;
        /* the sink suspended us while Windows keeps playing (multipoint): never */
        for (ULONG i = 0; i < 12000; ++i) edges += PnResumeDetect(&r, TRUE, FALSE, 10000);
        CHECK(edges == 0);
        /* paused on the receiver's remote for 3 s (the endpoint stays in RUN), then play */
        for (ULONG i = 0; i < 300; ++i) edges += PnResumeDetect(&r, TRUE, TRUE, 10000);
        CHECK(edges == 0);
        RED_R2("R4-7", PnResumeDetect(&r, TRUE, FALSE, 10000));
        CHECK(!PnResumeDetect(&r, TRUE, FALSE, 10000));   /* once per pause */
        /* a gap shorter than PN_RESUME_SILENCE_MS (gapless, a short fade) is no resume */
        r = {};
        for (ULONG i = 0; i + 1 < PN_RESUME_SILENCE_MS / 10; ++i) PnResumeDetect(&r, TRUE, TRUE, 10000);
        CHECK(!PnResumeDetect(&r, TRUE, FALSE, 10000));
        /* silence before the suspend does not count */
        r = {};
        for (ULONG i = 0; i < 300; ++i) PnResumeDetect(&r, FALSE, TRUE, 10000);
        CHECK(!PnResumeDetect(&r, TRUE, FALSE, 10000));
        /* the planner STARTs on that request even after the episode's timed START */
        PN_RECONCILE_PLAN p;
        PN_RECONCILE_INPUT in = TWant(AvdtpStateOpen);
        in.RemoteSuspended = TRUE; in.RemoteSuspendRetried = TRUE; in.RemoteSuspendAgeMs = 20000;
        PnPlanReconcile(&in, &p);
        CHECK(!PnPlanHas(&p, PnStepStartIfOpen));
        in.LocalStartRequested = TRUE;
        PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepStartIfOpen));
    }
}

/* Review round 5. #2: a receiver START crossing our RECONFIGURE is applied first (the
 * acceptor runs during our transaction); the receiver judged our RECONFIGURE in Open, so
 * its acceptance stands, as when HEAD's command lock held the START back. #3: the T3/T9
 * wait for the receiver's OPEN also at the OPEN itself. #6: a 2 Mb/s-only radio keeps the
 * remote's MTU for the pump, so the SBC packing still weighs the MTU-filling packet. */
static void TestTReview5()
{
    /* #2 */
    {
        TAcp a(AvdtpStateOpen, 1);
        a.ctx.IntSignal = AVDTP_RECONFIGURE;
        a.Run(AVDTP_START, { 1 << 2 });
        CHECK(a.Accepted() && a.r.NewState == AvdtpStateStreaming);   /* the crossing START */
        RED_R2("R5-2", PnIntOutcome(AVDTP_RECONFIGURE, 0, AvdtpStateOpen, AvdtpStateStreaming) == PnIntApply);
        CHECK(PnIntOutcome(AVDTP_RECONFIGURE, PN_ST_DEVICE_PROTOCOL_ERROR, AvdtpStateOpen, AvdtpStateStreaming) == PnIntLost);   /* refused: as HEAD */
        CHECK(PnIntOutcome(AVDTP_RECONFIGURE, PN_ST_IO_TIMEOUT, AvdtpStateOpen, AvdtpStateStreaming) == PnIntLost);
        CHECK(PnIntOutcome(AVDTP_RECONFIGURE, 0, AvdtpStateOpen, AvdtpStateOpen) == PnIntApply);
        CHECK(PnIntOutcome(AVDTP_RECONFIGURE, 0, AvdtpStateOpen, AvdtpStateClosing) == PnIntLost);
        CHECK(PnIntOutcome(AVDTP_SET_CONFIGURATION, 0, AvdtpStateIdle, AvdtpStateConfigured) == PnIntLost);   /* unchanged */
    }
    /* #3 */
    {
        RED_R2("R5-3", PnRemoteOpenWaitMs(TRUE, AvdtpStateConfigured, 20) == PN_REMOTE_OPEN_WAIT_MS - 20);
        CHECK(PnRemoteOpenWaitMs(TRUE, AvdtpStateConfigured, PN_REMOTE_OPEN_WAIT_MS - 10) == PN_PLAN_MIN_RETRY_MS);
        CHECK(PnRemoteOpenWaitMs(TRUE, AvdtpStateConfigured, PN_REMOTE_OPEN_WAIT_MS) == 0);
        CHECK(PnRemoteOpenWaitMs(FALSE, AvdtpStateConfigured, 20) == 0);   /* ours: OPEN at once */
        CHECK(PnRemoteOpenWaitMs(TRUE, AvdtpStateOpening, 20) == 0 && PnRemoteOpenWaitMs(TRUE, AvdtpStateIdle, 20) == 0);
        /* the planner's rule is the same function: an Idle snapshot plans the OPEN, the
         * live Configured state after a receiver SET_CONFIGURATION during discovery waits */
        PN_RECONCILE_PLAN p;
        PN_RECONCILE_INPUT in = TWant(AvdtpStateIdle);
        PnPlanReconcile(&in, &p);
        CHECK(PnPlanHas(&p, PnStepEnsureOpen) && !PnPlanHas(&p, PnStepWaitRemoteOpen));
        in = TWant(AvdtpStateConfigured); in.RemoteConfigured = TRUE; in.RemoteConfiguredAgeMs = 20;
        PnPlanReconcile(&in, &p);
        CHECK(TPlanIs(p, { PnStepWaitRemoteOpen }) && p.RetryDelayMs == PnRemoteOpenWaitMs(TRUE, AvdtpStateConfigured, 20));
    }
    /* #6 */
    {
        const ULONGLONG edr2 = (1ull << 25) | (1ull << 39) | (1ull << 40), edr3 = 1ull << 26;   /* with 3- and 5-slot EDR packets */
        RED_R2("R5-6", PnPumpMediaMtuCap(1, 3 | edr2) == PN_MEDIA_MTU_NO_CAP);
        CHECK(PnPumpMediaMtuCap(1, 3 | edr2 | edr3) == 1017 && PnPumpMediaMtuCap(1, 3) == 1013 && PnPumpMediaMtuCap(0, 0) == 1017);
        CHECK(PnPreferredMediaMtu(1, 3 | edr2) == 675);   /* the MTU we ask for stays */
        /* Sony dual 53 (224-byte frames, MTU 895) on a 2 Mb/s-only radio: 3 frames, as HEAD */
        const ULONG hdr = 13, frameUs = 128u * 1000000u / 48000u;
        ULONG mtu = PnEffectiveMediaMtu(895, PnPumpMediaMtuCap(1, 3 | edr2), 672);
        RED_R2("R5-6", mtu == 895 && PnSbcPacking(224, frameUs, 60, mtu - hdr, hdr, PnBasebandSduBytes(TRUE, 3 | edr2)) == 3);
        /* 3 Mb/s radio: unchanged (3 frames, one 3-DH5) */
        mtu = PnEffectiveMediaMtu(895, PnPumpMediaMtuCap(1, 3 | edr2 | edr3), 672);
        CHECK(mtu == 895 && PnSbcPacking(224, frameUs, 60, mtu - hdr, hdr, PnBasebandSduBytes(TRUE, 3 | edr2 | edr3)) == 3);
        CHECK(PnEffectiveMediaMtu(1024, PnPumpMediaMtuCap(1, 3 | edr2 | edr3), 672) == 1017);
    }
}

/* ---- 0.1.0.22 #1: the sample-rate converter in the selection and format policy ------------ */

/* The 0.1.0.21 rules (C8: AllowHalfRate), for the NONE / HALF differential below. */
static ULONG TOldStreamRateFor021(ULONG SinkRateMask, ULONG EngineRate, BOOLEAN AllowHalfRate)
{
    if ((SinkRateMask & A2dpHzToRateMask(EngineRate)) || !AllowHalfRate) return EngineRate;
    if ((EngineRate == 96000 || EngineRate == 88200) && (SinkRateMask & A2dpHzToRateMask(EngineRate / 2))) return EngineRate / 2;
    return EngineRate;
}

static PN_FORMAT_ACTION TOldPlanStreamFormat021(const PN_FORMAT_INPUT *In, BOOLEAN AllowHalfRate, A2DP_CODEC_CONFIG *Cfg)
{
    AVDTP_STREAM_STATE st = In->StreamState;
    if (!In->ActiveValid || !In->AudioStreamOpen ||
        (st != AvdtpStateOpen && st != AvdtpStateStreaming && st != AvdtpStateConfigured)) return PnFormatKeep;
    *Cfg = *In->Active;
    const AVDTP_REMOTE_SEP *r = In->Remote;
    A2DP_CODEC_CAPS caps = {};
    BOOLEAN parsed = r && A2dpParseCaps(r->MediaCodecType, r->CodecCaps, r->CodecCapsLength, &caps, NULL, NULL);
    ULONG rate = parsed ? TOldStreamRateFor021(caps.RateMask, In->AudioRate, AllowHalfRate) : In->AudioRate;
    BOOLEAN selected = parsed && (caps.RateMask & A2dpHzToRateMask(rate));
    if (selected && Cfg->Codec == PNA2DP_CODEC_SBC) {
        caps.RateMask &= A2dpHzToRateMask(rate);
        selected = A2dpSelectConfig(&caps, In->Config, rate, In->AudioValidBits, Cfg);
        if (selected && In->Conservative) selected = A2dpConstrainBasicRadio(Cfg);
    } else if (selected) {
        Cfg->SampleRate = rate;
        A2dpSerializeConfig(Cfg);
    }
    if (selected && PnStreamFormatNeedsUpdate(In->Active->Codec, In->Active->SampleRate, In->Active->ChannelMode, Cfg->SampleRate, Cfg->ChannelMode))
        return (In->RemoteConfigured && st == AvdtpStateConfigured) ? PnFormatWaitOpenReconfigure : PnFormatReconfigure;
    if (!selected && In->Active->SampleRate != rate) return PnFormatReselect;
    return PnFormatKeep;
}

/* A vendor-codec sink SEP with its own rate / channel byte (aptX family, LDAC). */
static AVDTP_REMOTE_SEP TVendorSinkSep(UCHAR Seid, ULONG Vendor, USHORT CodecId, UCHAR Byte6, UCHAR Byte7, ULONG Length)
{
    UCHAR ies[16] = {}, caps[64];
    ies[0] = (UCHAR)Vendor; ies[1] = (UCHAR)(Vendor >> 8); ies[2] = (UCHAR)(Vendor >> 16); ies[3] = (UCHAR)(Vendor >> 24);
    ies[4] = (UCHAR)CodecId; ies[5] = (UCHAR)(CodecId >> 8); ies[6] = Byte6; ies[7] = Byte7;
    return TSepFromCaps(Seid, FALSE, caps, TCapsBytes(caps, ies, Length, A2DP_CODEC_VENDOR, FALSE));
}

static void TestTRateConvert()
{
    const ULONG both = PNA2DP_RATE_44100 | PNA2DP_RATE_48000;
    /* the stream rate: the engine rate whenever the sink takes it, else what the converter bridges */
    CHECK(PnStreamRateFor(both, 96000, PN_RATECONV_ANY) == 48000);
    CHECK(PnStreamRateFor(PNA2DP_RATE_44100, 96000, PN_RATECONV_ANY) == 44100);
    CHECK(PnStreamRateFor(PNA2DP_RATE_48000, 44100, PN_RATECONV_ANY) == 48000);
    CHECK(PnStreamRateFor(PNA2DP_RATE_44100, 48000, PN_RATECONV_ANY) == 44100);
    CHECK(PnStreamRateFor(both, 88200, PN_RATECONV_ANY) == 44100);
    CHECK(PnStreamRateFor(PNA2DP_RATE_48000, 88200, PN_RATECONV_ANY) == 48000);
    CHECK(PnStreamRateFor(both, 44100, PN_RATECONV_ANY) == 44100 && PnStreamRateFor(both, 48000, PN_RATECONV_ANY) == 48000);
    CHECK(PnStreamRateFor(both | PNA2DP_RATE_96000, 96000, PN_RATECONV_ANY) == 96000);   /* native first */
    CHECK(PnStreamRateFor(PNA2DP_RATE_96000, 48000, PN_RATECONV_ANY) == 48000);           /* no conversion up: the selection fails */
    CHECK(PnStreamRateFor(PNA2DP_RATE_48000, 32000, PN_RATECONV_ANY) == 32000);           /* 32 kHz engines are not bridged */
    CHECK(PnStreamRateFor(PNA2DP_RATE_48000, 192000, PN_RATECONV_ANY) == 192000);
    /* NONE and HALF are the 0.1.0.21 rules exactly */
    {
        static const ULONG masks[] = { both, PNA2DP_RATE_44100, PNA2DP_RATE_48000, PNA2DP_RATE_96000, PNA2DP_RATE_32000 | PNA2DP_RATE_16000,
                                       both | PNA2DP_RATE_88200 | PNA2DP_RATE_96000, PNA2DP_RATE_88200, 0 };
        static const ULONG engines[] = { 16000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
        BOOLEAN same = TRUE;
        for (ULONG m : masks) for (ULONG e : engines)
            same = same && PnStreamRateFor(m, e, PN_RATECONV_NONE) == TOldStreamRateFor021(m, e, FALSE) &&
                   PnStreamRateFor(m, e, PN_RATECONV_HALF) == TOldStreamRateFor021(m, e, TRUE);
        CHECK(same);
    }
    PN_CODEC_CHOICE c;
    /* the codec follows the priority list, not the Windows rate: an aptX HD sink at 48 kHz only */
    {
        TChoiceEnv e;
        e.cfg.CodecPriority[0] = PNA2DP_CODEC_APTX_HD; e.cfg.CodecPriority[1] = PNA2DP_CODEC_SBC; e.cfg.CodecPriority[2] = 0;
        e.cfg.EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD);
        e.Add(TVendorSinkSep(1, A2DP_VENDOR_APTX_HD, A2DP_CODECID_APTX_HD, APTX_FREQ_48000 | APTX_CH_STEREO, 0, 11));
        e.Add(TSbcSinkSep(2, 0x7f));   /* 32 / 44.1 / 48 kHz */
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD);
        e.in.RestrictRate = TRUE; e.in.PreferredRate = 44100; e.in.PreferredBits = 24;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.Cfg.SampleRate == 44100);   /* 0.1.0.21: aptX HD skipped */
        e.in.RateConvert = PN_RATECONV_ANY;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_APTX_HD && c.Cfg.SampleRate == 48000 && c.RemoteSeid == 1);
        e.in.PreferredRate = 96000;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_APTX_HD && c.Cfg.SampleRate == 48000);
        e.in.PreferredRate = 32000;   /* not bridged: SBC at the engine rate, as before */
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_SBC && c.Cfg.SampleRate == 32000);
    }
    /* an LDAC sink at 44.1 / 48 kHz (the EDIFIER MR5): the engine rate when it has it; a converted
     * 16-bit engine feeds LDAC S32, not a requantised S16 */
    {
        TChoiceEnv e;   /* LDAC, AAC, SBC */
        e.Add(TVendorSinkSep(1, A2DP_VENDOR_SONY, A2DP_CODECID_LDAC, LDAC_FREQ_44100 | LDAC_FREQ_48000, LDAC_CH_STEREO, 8));
        e.Add(TSbcSinkSep(2, 0x3f));
        e.in.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
        e.in.RestrictRate = TRUE; e.in.RateConvert = PN_RATECONV_ANY; e.in.PreferredBits = 16;
        e.in.PreferredRate = 44100;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_LDAC && c.Cfg.SampleRate == 44100 && c.Cfg.PcmFormat == PNA2DP_FMT_S16);
        e.in.PreferredRate = 48000;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_LDAC && c.Cfg.SampleRate == 48000 && c.Cfg.PcmFormat == PNA2DP_FMT_S16);
        e.in.PreferredRate = 96000;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_LDAC && c.Cfg.SampleRate == 48000 && c.Cfg.PcmFormat == PNA2DP_FMT_S32);
        e.in.PreferredRate = 88200;
        CHECK(e.Choose(&c) && c.Cfg.Codec == PNA2DP_CODEC_LDAC && c.Cfg.SampleRate == 44100 && c.Cfg.PcmFormat == PNA2DP_FMT_S32);
        e.in.RateConvert = PN_RATECONV_NONE; e.in.PreferredRate = 96000;
        CHECK(!e.Choose(&c));   /* 0.1.0.21 without C8: nothing carries 96 kHz */
    }
    /* a Windows format change the pump bridges keeps the stream: no RECONFIGURE */
    {
        PNA2DP_CONFIG cfg = TConfig();
        AVDTP_REMOTE_SEP sbc = TSbcSinkSep(1, 0x3f);
        UCHAR offer44[] = { 0x21, 0x15, 2, 53 };   /* SBC 44.1 kHz joint stereo */
        A2DP_CODEC_CONFIG active, out;
        CHECK(A2dpParseConfig(A2DP_CODEC_SBC, offer44, 4, &active));
        PN_FORMAT_INPUT in;
        memset(&in, 0, sizeof(in));
        in.ActiveValid = TRUE; in.Active = &active; in.Remote = &sbc; in.Config = &cfg; in.AudioStreamOpen = TRUE;
        in.AudioRate = 48000; in.AudioValidBits = 16; in.StreamState = AvdtpStateOpen;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000);   /* NONE: as before */
        in.RateConvert = PN_RATECONV_HALF;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000);
        in.RateConvert = PN_RATECONV_ANY;
        /* review #4: a stream that has carried no audio since it was configured (ours, configured
         * with no audio open, e.g. auto start on connect) goes to the engine rate before its START */
        RED("R22-bridge", PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000);
        in.Streamed = TRUE;                                      /* it has played: the format change is bridged */
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
        in.StreamState = AvdtpStateConfigured; in.RemoteConfigured = TRUE; in.Streamed = FALSE;   /* the receiver configured 44.1 kHz under a 48 kHz engine */
        RED("R22-bridge", PnPlanStreamFormat(&in, &out) == PnFormatWaitOpenReconfigure && out.SampleRate == 48000);
        in.StreamState = AvdtpStateStreaming; in.RemoteConfigured = FALSE; in.Streamed = TRUE;
        in.Remote = NULL;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);   /* remote unknown, the pair bridgeable */
        AVDTP_REMOTE_SEP sbc32 = TSbcSinkSep(1, 0x7f);          /* 32 kHz is not bridged; this sink takes it */
        in.Remote = &sbc32; in.AudioRate = 32000;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 32000);
        in.Remote = &sbc;                                        /* the 44.1 / 48 kHz sink does not: another codec */
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReselect);
        /* review #1/#3: a RECONFIGURE needed anyway (a channel-mode preference) lands on the engine
         * rate, not on the bridged one: it costs the same and ends the conversion */
        in.AudioRate = 48000; cfg.Flags = PNA2DP_CFG_DUAL_CHANNEL_PREFER;
        RED("R22-bridge", PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 48000 && out.ChannelMode == PNA2DP_CHMODE_DUAL);
        cfg.Flags = 0;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);   /* without one: bridged */
    }
    /* review #4: an LDAC stream we configured at 48 kHz with no audio open (auto start on connect),
     * then Windows opens at 44.1 kHz: native 44.1 kHz before the first START; once it has played,
     * a later change to 44.1 kHz is bridged */
    {
        PNA2DP_CONFIG cfg = TConfig(), pref = {};
        AVDTP_REMOTE_SEP ldac = TVendorSinkSep(1, A2DP_VENDOR_SONY, A2DP_CODECID_LDAC, LDAC_FREQ_44100 | LDAC_FREQ_48000, LDAC_CH_STEREO, 8);
        A2DP_CODEC_CAPS caps;
        A2DP_CODEC_CONFIG active, out;
        CHECK(A2dpParseCaps(ldac.MediaCodecType, ldac.CodecCaps, ldac.CodecCapsLength, &caps, NULL, NULL) && A2dpSelectConfig(&caps, &pref, 48000, 16, &active));
        PN_FORMAT_INPUT in;
        memset(&in, 0, sizeof(in));
        in.ActiveValid = TRUE; in.Active = &active; in.Remote = &ldac; in.Config = &cfg; in.AudioStreamOpen = TRUE;
        in.AudioRate = 44100; in.AudioValidBits = 16; in.StreamState = AvdtpStateOpen; in.RateConvert = PN_RATECONV_ANY;
        RED("R22-autostart", PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 44100 && out.Codec == PNA2DP_CODEC_LDAC);
        in.Streamed = TRUE;
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
    }
    /* a hi-res engine rate the sink takes natively gets its own stream; one it lacks is bridged */
    {
        PNA2DP_CONFIG cfg = TConfig(), pref = {};
        AVDTP_REMOTE_SEP ldac = TSinkSep(1, PNA2DP_CODEC_LDAC), hd = TSinkSep(2, PNA2DP_CODEC_APTX_HD);
        A2DP_CODEC_CAPS caps;
        A2DP_CODEC_CONFIG activeLdac, activeHd, out;
        CHECK(A2dpParseCaps(ldac.MediaCodecType, ldac.CodecCaps, ldac.CodecCapsLength, &caps, NULL, NULL) && A2dpSelectConfig(&caps, &pref, 48000, 24, &activeLdac));
        CHECK(A2dpParseCaps(hd.MediaCodecType, hd.CodecCaps, hd.CodecCapsLength, &caps, NULL, NULL) && A2dpSelectConfig(&caps, &pref, 48000, 24, &activeHd));
        PN_FORMAT_INPUT in;
        memset(&in, 0, sizeof(in));
        in.ActiveValid = TRUE; in.Active = &activeLdac; in.Remote = &ldac; in.Config = &cfg; in.AudioStreamOpen = TRUE;
        in.AudioRate = 96000; in.AudioValidBits = 24; in.StreamState = AvdtpStateStreaming; in.RateConvert = PN_RATECONV_ANY;
        in.Streamed = TRUE;   /* a format change on a stream that has played */
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatReconfigure && out.SampleRate == 96000 && out.Codec == PNA2DP_CODEC_LDAC);
        in.AudioRate = 44100;   /* LDAC takes 44.1 kHz too, but 44.1 -> 48 kHz is bridged: no interruption */
        CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep);
        in.Active = &activeHd; in.Remote = &hd;
        for (ULONG r : { 96000u, 88200u, 44100u }) { in.AudioRate = r; CHECK(PnPlanStreamFormat(&in, &out) == PnFormatKeep); }
    }
    /* NONE and HALF: the 0.1.0.21 decisions, over random sinks, streams and engine formats */
    {
        static const ULONG codecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC };
        static const UCHAR sbcFreqModes[] = { 0x3f, 0x2f, 0x1f, 0x11, 0x21, 0x12, 0x14, 0x38, 0xf1 };
        static const ULONG rates[] = { 44100, 48000, 96000, 88200, 32000, 16000 };
        unsigned diff = 0, compared = 0, acts[4] = {};
        for (unsigned it = 0; it < 20000; it++) {
            ULONG codec = codecs[TRnd() % 6];
            AVDTP_REMOTE_SEP remote = (codec == PNA2DP_CODEC_SBC && (TRnd() & 1)) ? TSbcSinkSep(1, sbcFreqModes[TRnd() % 9]) : TSinkSep(1, codec);
            A2DP_CODEC_CAPS caps;
            A2DP_CODEC_CONFIG active, o1, o2;
            PNA2DP_CONFIG pref = {}, cfg = {};
            cfg.Flags = TRnd() & (PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_AUTO_QUALITY);
            if (!A2dpParseCaps(remote.MediaCodecType, remote.CodecCaps, remote.CodecCapsLength, &caps, NULL, NULL) ||
                !A2dpSelectConfig(&caps, &pref, rates[TRnd() % 6], 16, &active)) continue;
            PN_FORMAT_INPUT in;
            memset(&in, 0, sizeof(in));
            in.ActiveValid = (TRnd() % 8) != 0; in.Active = &active; in.Remote = (TRnd() % 8) ? &remote : NULL; in.Config = &cfg;
            in.AudioStreamOpen = (TRnd() % 8) != 0; in.AudioRate = rates[TRnd() % 6]; in.AudioValidBits = (TRnd() & 1) ? 16 : 24;
            in.Conservative = (TRnd() % 4) == 0; in.StreamState = (AVDTP_STREAM_STATE)(TRnd() % 7); in.RemoteConfigured = TRnd() & 1;
            in.Streamed = TRnd() & 1;   /* matters only to the converter (ANY) */
            BOOLEAN half = TRnd() & 1;
            in.RateConvert = half ? PN_RATECONV_HALF : PN_RATECONV_NONE;
            memset(&o1, 0, sizeof(o1)); memset(&o2, 0, sizeof(o2));
            PN_FORMAT_ACTION a = PnPlanStreamFormat(&in, &o1), b = TOldPlanStreamFormat021(&in, half, &o2);
            compared++;
            acts[a]++;
            if (a != b || ((a == PnFormatReconfigure || a == PnFormatWaitOpenReconfigure) && memcmp(&o1, &o2, sizeof(o1)) != 0)) diff++;
        }
        CHECK(diff == 0 && compared > 15000 && acts[PnFormatReconfigure] > 100 && acts[PnFormatReselect] > 100);
    }
    /* ANY (review #1/#3/#4), over random sinks, streams and engine formats: every RECONFIGURE lands
     * on the engine's own stream rate (PnStreamRateFor); a stream that has carried no audio is
     * kept only at that rate; one that has played is kept at another rate only when it is bridged */
    {
        static const ULONG codecs[] = { PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC };
        static const UCHAR sbcFreqModes[] = { 0x3f, 0x2f, 0x1f, 0x11, 0x21, 0x12, 0x14, 0x38, 0xf1 };
        static const ULONG rates[] = { 44100, 48000, 96000, 88200, 32000, 16000 };
        unsigned bad = 0, keptBridged = 0, reconf = 0, keptUnplayed = 0;
        for (unsigned it = 0; it < 20000; it++) {
            ULONG codec = codecs[TRnd() % 6];
            AVDTP_REMOTE_SEP remote = (codec == PNA2DP_CODEC_SBC && (TRnd() & 1)) ? TSbcSinkSep(1, sbcFreqModes[TRnd() % 9]) : TSinkSep(1, codec);
            A2DP_CODEC_CAPS caps;
            A2DP_CODEC_CONFIG active, o;
            PNA2DP_CONFIG pref = {}, cfg = {};
            cfg.Flags = TRnd() & (PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_AUTO_QUALITY);
            if (!A2dpParseCaps(remote.MediaCodecType, remote.CodecCaps, remote.CodecCapsLength, &caps, NULL, NULL) ||
                !A2dpSelectConfig(&caps, &pref, rates[TRnd() % 6], 16, &active)) continue;
            PN_FORMAT_INPUT in;
            memset(&in, 0, sizeof(in));
            in.ActiveValid = TRUE; in.Active = &active; in.Remote = &remote; in.Config = &cfg; in.AudioStreamOpen = TRUE;
            in.AudioRate = rates[TRnd() % 6]; in.AudioValidBits = (TRnd() & 1) ? 16 : 24; in.Conservative = (TRnd() % 4) == 0;
            static const AVDTP_STREAM_STATE states[] = { AvdtpStateConfigured, AvdtpStateOpen, AvdtpStateStreaming };
            in.StreamState = states[TRnd() % 3]; in.RemoteConfigured = TRnd() & 1; in.Streamed = TRnd() & 1;
            in.RateConvert = PN_RATECONV_ANY;
            ULONG want = PnStreamRateFor(caps.RateMask, in.AudioRate, PN_RATECONV_ANY);
            PN_FORMAT_ACTION a = PnPlanStreamFormat(&in, &o);
            if (a == PnFormatReconfigure || a == PnFormatWaitOpenReconfigure) { reconf++; bad += o.SampleRate != want; }
            if (a == PnFormatKeep && active.SampleRate != want && (caps.RateMask & A2dpHzToRateMask(want))) {
                if (in.Streamed && PnRateConvertible(in.AudioRate, active.SampleRate, PN_RATECONV_ANY)) keptBridged++;
                else bad++;
            }
            if (a == PnFormatKeep && !in.Streamed) keptUnplayed++;
        }
        RED("R22-bridge", bad == 0);
        CHECK(reconf > 1000 && keptBridged > 500 && keptUnplayed > 500);
    }
}

static void TestLaneT()
{
    unsigned before = failures;
    TestTChooser();
    TestTPlanner();
    TestTFormat();
    TestTAcceptor();
    TestTDiscoveryAndSdp();
    TestTSmallPolicies();
    TestTDifferential();
    TestTRedItems();
    TestTRedScenarios();
    TestTSonySbcPath();
    TestTLocking();
    TestTReview1();
    TestTReview2();
    TestTReview4();
    TestTReview5();
    TestTRateConvert();
    tNonRedFailures = failures - before - tRedFailures;
    printf("lane T: %u non-RED failures; RED group (corrected transport): %u of %u checks failing\n",
           tNonRedFailures, tRedFailures, tRedChecks);
}
