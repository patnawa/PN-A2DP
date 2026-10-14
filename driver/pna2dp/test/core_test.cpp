/* Tests the production codec negotiation and RTP helpers. SPDX-License-Identifier: MIT */
#include "codec_core.h"
#include "pcm.h"
#include "avdtp_wire.h"
#include "audio_policy.h"
#include "config_policy.h"
#include <stdio.h>
#include "../../../common/recovery_policy.h"
#include <vector>
#include <cmath>

static unsigned failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
#include "core_test_S.inl"
#include "core_test_W.inl"

static void TestCodecNegotiation()
{
    PNA2DP_CONFIG pref = {};
    A2DP_CODEC_CAPS caps;
    A2DP_CODEC_CONFIG cfg, parsed;
    UCHAR ie[32], type;
    ULONG n = A2dpBuildLocalCaps(PNA2DP_CODEC_AAC, ie, sizeof(ie), &type);
    CHECK(A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
    CHECK(caps.AacMaxBitrate == 320000);
    {
        A2DP_CODEC_CAPS unrestricted = caps;
        unrestricted.AacMaxBitrate = 0;
        pref.Aac.BitrateBps = 320000;
        CHECK(A2dpSelectConfig(&unrestricted, &pref, 48000, 16, &cfg) && cfg.BitrateBps == 320000);
        unrestricted.AacMaxBitrate = 192000;
        CHECK(A2dpSelectConfig(&unrestricted, &pref, 48000, 16, &cfg) && cfg.BitrateBps == 192000);
        unrestricted.AacMaxBitrate = 0; pref.Aac.BitrateBps = 999000;
        CHECK(A2dpSelectConfig(&unrestricted, &pref, 48000, 16, &cfg) && cfg.BitrateBps == 320000);
        pref.Aac.BitrateBps = 0;
    }

    // An MPEG-4-only speaker must never be sent an MPEG-2 configuration.
    ie[0] = AAC_OBJ_MPEG4_LC;
    CHECK(A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
    CHECK(A2dpSelectConfig(&caps, &pref, 48000, 16, &cfg));
    CHECK(cfg.Ies[0] == AAC_OBJ_MPEG4_LC);
    CHECK(A2dpParseConfig(type, cfg.Ies, cfg.IesLength, &parsed));
    A2dpSerializeConfig(&parsed);
    CHECK(parsed.Ies[0] == AAC_OBJ_MPEG4_LC);
    A2DP_CODEC_CAPS unsupportedRate = caps;
    unsupportedRate.RateMask = PNA2DP_RATE_16000;
    CHECK(!A2dpSelectConfig(&unsupportedRate, &pref, 16000, 16, &cfg));
    ie[0] = AAC_OBJ_MPEG4_LTP;
    CHECK(A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
    CHECK(!A2dpSelectConfig(&caps, &pref, 48000, 16, &cfg));

    // SBC XQ must respect the bitpool limit of the selected mode/subbands.
    n = A2dpBuildLocalCaps(PNA2DP_CODEC_SBC, ie, sizeof(ie), &type);
    CHECK(A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
    pref.Sbc.ChannelModeMask = PNA2DP_SBC_CH_DUAL;
    CHECK(A2dpSelectConfig(&caps, &pref, 48000, 16, &cfg));
    CHECK(cfg.SbcMaxBitpool <= 128 && cfg.BitrateBps != 0);
    pref.Sbc.SubbandMask = PNA2DP_SBC_SUBBANDS_4;
    CHECK(A2dpSelectConfig(&caps, &pref, 48000, 16, &cfg));
    CHECK(cfg.SbcMaxBitpool <= 64 && cfg.BitrateBps != 0);
    caps.SbcMaxBitpool = 53;
    pref.Sbc.MinBitpool = 80;
    CHECK(!A2dpSelectConfig(&caps, &pref, 48000, 16, &cfg));

    UCHAR invalidSbc[][4] = {
        {0x31, 0x15, 2, 53}, // multiple rates
        {0x13, 0x15, 2, 53}, // multiple modes
        {0x11, 0x05, 2, 53}, // no blocks
        {0x11, 0x11, 2, 53}, // no subbands
        {0x11, 0x14, 2, 53}, // no allocation method
        {0x11, 0x15, 54, 53},
        {0x11, 0x15, 0, 53},
        {0x14, 0x15, 2, 250}, // dual: max 128
    };
    for (const auto &bad : invalidSbc) CHECK(!A2dpParseConfig(A2DP_CODEC_SBC, bad, sizeof(bad), &cfg));

    // A receiver can initiate a joint-stereo stream even when the saved setting
    // prefers dual channel. Reconcile must apply the same supported format as a
    // locally initiated stream, including when the sample rate already matches.
    {
        UCHAR sonyCaps[] = {0x3f, 0xff, 2, 53};
        UCHAR remoteOffer[] = {0x11, 0x15, 2, 53};
        PNA2DP_CONFIG saved = {}; saved.Flags = PNA2DP_CFG_DUAL_CHANNEL_PREFER;
        A2DP_CODEC_CONFIG active, selected;
        CHECK(A2dpParseCaps(A2DP_CODEC_SBC, sonyCaps, sizeof(sonyCaps), &caps, NULL, NULL));
        CHECK(A2dpParseConfig(A2DP_CODEC_SBC, remoteOffer, sizeof(remoteOffer), &active));
        CHECK(A2dpSelectConfig(&caps, &saved, 48000, 16, &selected));
        CHECK(active.BitrateBps == 357000 && selected.BitrateBps == 672000);
        CHECK(PnStreamFormatNeedsUpdate(active.Codec, active.SampleRate, active.ChannelMode, selected.SampleRate, selected.ChannelMode));
        CHECK(!PnStreamFormatNeedsUpdate(selected.Codec, selected.SampleRate, selected.ChannelMode, selected.SampleRate, selected.ChannelMode));
        CHECK(PnStreamFormatNeedsUpdate(active.Codec, 44100, active.ChannelMode, 48000, active.ChannelMode));
        caps.ChannelModeMask = PNA2DP_CHMODE_JOINT;
        CHECK(A2dpSelectConfig(&caps, &saved, 48000, 16, &selected));
        CHECK(!PnStreamFormatNeedsUpdate(active.Codec, active.SampleRate, active.ChannelMode, selected.SampleRate, selected.ChannelMode));
        CHECK(!PnStreamFormatNeedsUpdate(PNA2DP_CODEC_AAC, 48000, PNA2DP_CHMODE_STEREO, 48000, PNA2DP_CHMODE_MONO));
    }

    // Every advertised codec selects and round-trips at each supported rate.
    pref = {};
    for (ULONG codec = PNA2DP_CODEC_SBC; codec <= PNA2DP_CODEC_LDAC; ++codec) {
        n = A2dpBuildLocalCaps(codec, ie, sizeof(ie), &type);
        CHECK(n > 0 && A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
        for (ULONG bit = 1; bit <= PNA2DP_RATE_96000; bit <<= 1) {
            if (!(caps.RateMask & bit)) continue;
            CHECK(A2dpSelectConfig(&caps, &pref, A2dpRateMaskToHz(bit), 16, &cfg));
            CHECK(A2dpParseConfig(type, cfg.Ies, cfg.IesLength, &parsed));
            CHECK(parsed.SampleRate == cfg.SampleRate && parsed.ChannelMode == cfg.ChannelMode);
            CHECK(cfg.BitrateBps > 0);
            for (ULONG len = 0; len < n; ++len) CHECK(!A2dpParseConfig(type, cfg.Ies, len, &parsed));
        }
    }
}

static void TestRtp()
{
    RTP_STATE r = {};
    r.Sequence = 65535; r.Timestamp = 0xffffff80; r.Ssrc = 0x12345678; r.PayloadType = 96;
    UCHAR b[32] = {};
    RtpWriteHeader(&r, b, 256, TRUE);
    USHORT seq; ULONG ts, len;
    CHECK(r.Sequence == 0 && r.Timestamp == 128);
    CHECK(RtpParseHeader(b, 12, &seq, &ts, &len));
    CHECK(seq == 65535 && ts == 0xffffff80 && len == 12 && b[1] == 0xe0);
    for (ULONG i = 0; i < 12; ++i) CHECK(!RtpParseHeader(b, i, &seq, &ts, &len));
    b[0] = 0x91; b[18] = 0; b[19] = 1; // one CSRC + extension header + one word
    CHECK(!RtpParseHeader(b, 23, &seq, &ts, &len));
    CHECK(RtpParseHeader(b, 24, &seq, &ts, &len) && len == 24);
    // An AAC access unit fragmented over RTP shares a timestamp; only its final packet is marked.
    ULONG fragmentTimestamp = r.Timestamp;
    for (unsigned fragment = 0; fragment < 3; ++fragment) {
        RtpWriteHeader(&r, b, fragment == 2 ? 1024 : 0, fragment == 2);
        CHECK(RtpParseHeader(b, 12, &seq, &ts, &len) && ts == fragmentTimestamp);
        CHECK((b[1] >> 7) == (fragment == 2));
    }
    CHECK(r.Timestamp == fragmentTimestamp + 1024);
}

static void TestPcm()
{
    CHECK(PnVolumeGainQ15(LONG_MIN) == 0);
    CHECK(PnVolumeGainQ15(-96 * 65536) == 0);
    CHECK(PnVolumeGainQ15(LONG_MAX) == 32768);
    for (LONG step = 0; step < 192; ++step) {
        ULONG gain = PnVolumeGainQ15(-step * 32768);
        CHECK(std::abs(double(gain) - 32768.0 * std::pow(10.0, -double(step) / 40.0)) <= 0.5);
        SHORT source[] = {20000, -20000}, attenuated[2] = {};
        CHECK(PnConvertPcm((UCHAR*)source, sizeof(source), 2, 16, 16, FALSE,
            (UCHAR*)attenuated, sizeof(attenuated), PNA2DP_FMT_S16, 2, gain) == sizeof(attenuated));
        CHECK(std::abs(double(attenuated[0]) - 20000.0 * gain / 32768.0) <= 0.5);
        CHECK(std::abs(double(attenuated[1]) + 20000.0 * gain / 32768.0) <= 0.5);
    }
    /* NaN and +-Inf are silence: an Inf was a full-scale one-sample pop (quality harness) */
    ULONG floats[] = {0, 0x3f800000, 0xbf800000, 0x3f000000, 0xbf000000, 0x7fc00000, 0x7f800000, 0xff800000, 1};
    LONG expected[] = {0, 2147483647, (-2147483647L - 1), 1073741824, -1073741824, 0, 0, 0, 0};
    LONG out[9] = {};
    CHECK(PnConvertPcm((UCHAR*)floats, sizeof(floats), 1, 32, 32, TRUE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == sizeof(out));
    CHECK(memcmp(out, expected, sizeof(out)) == 0);
    SHORT stereo[] = {32767, -32767, 0, 20000, -30000, -30000};
    SHORT mono[4] = {0, 0, 0, 1234};
    CHECK(PnConvertPcm((UCHAR*)stereo, sizeof(stereo), 2, 16, 16, FALSE, (UCHAR*)mono, 6, PNA2DP_FMT_S16, 1, 32768) == 6);
    CHECK(mono[0] == 0 && mono[1] == 10000 && mono[2] == -30000 && mono[3] == 1234);
    CHECK(PnConvertPcm((UCHAR*)stereo, sizeof(stereo), 2, 16, 16, FALSE, (UCHAR*)mono, 6, PNA2DP_FMT_S16, 1, 0) == 6);
    CHECK(mono[0] == 0 && mono[1] == 0 && mono[2] == 0);
    CHECK(PnConvertPcm((UCHAR*)stereo, sizeof(stereo), 0, 16, 16, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 0);
    CHECK(PnConvertPcm((UCHAR*)stereo, sizeof(stereo), 2, 8, 8, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 0);
    UCHAR packed[] = {0x00,0x00,0x80, 0xff,0xff,0x7f};
    CHECK(PnConvertPcm(packed, sizeof(packed), 1, 24, 24, FALSE, (UCHAR*)out, sizeof(out), PNA2DP_FMT_S32, 1, 32768) == 8);
    CHECK(out[0] == (-2147483647L - 1) && out[1] == 0x7fffff00);
}

static void TestAvdtp()
{
    AVDTP_REASSEMBLY r = {};
    UCHAR start[] = {0x14, 3, AVDTP_SET_CONFIGURATION, 1, 2};
    UCHAR cont[] = {0x18, 3, 4}, end[] = {0x1c, 5, 6};
    CHECK(AvdtpReassemble(&r, start, sizeof(start)) == AvdtpMore);
    CHECK(AvdtpReassemble(&r, cont, sizeof(cont)) == AvdtpMore);
    CHECK(AvdtpReassemble(&r, end, sizeof(end)) == AvdtpComplete);
    CHECK(r.Length == 6 && memcmp(r.Buffer, "\1\2\3\4\5\6", 6) == 0 && !r.Active);
    CHECK(AvdtpReassemble(&r, end, sizeof(end)) == AvdtpInvalid);
    AvdtpReassemble(&r, start, sizeof(start));
    CHECK(AvdtpReassemble(&r, end, sizeof(end)) == AvdtpInvalid); // premature END
    AvdtpReassemble(&r, start, sizeof(start));
    cont[0] = 0x1a;
    CHECK(AvdtpReassemble(&r, cont, sizeof(cont)) == AvdtpInvalid); // different message type
    cont[0] = 0x28;
    AvdtpReassemble(&r, start, sizeof(start));
    CHECK(AvdtpReassemble(&r, cont, sizeof(cont)) == AvdtpInvalid); // different transaction
    UCHAR huge[AVDTP_MAX_SIGNAL_PAYLOAD + 4] = {};
    CHECK(AvdtpReassemble(&r, huge, sizeof(huge)) == AvdtpInvalid);
    start[1] = 1;
    CHECK(AvdtpReassemble(&r, start, sizeof(start)) == AvdtpInvalid);
    UCHAR caps[] = {1,0, 7,6,0,0, 0x11,0x15,2,53, 8,0};
    AVDTP_REMOTE_SEP sep = {};
    CHECK(AvdtpParseServiceCapabilities(caps, sizeof(caps), &sep));
    CHECK(sep.Codec == PNA2DP_CODEC_SBC && sep.ServiceCaps == (PNA2DP_SVC_MEDIA_TRANSPORT | PNA2DP_SVC_DELAY_REPORTING));
    CHECK(!AvdtpParseServiceCapabilities(caps, sizeof(caps)-1, &sep) && !sep.CapsValid);
    caps[10] = 1; // duplicate media transport category
    CHECK(!AvdtpParseServiceCapabilities(caps, sizeof(caps), &sep) && !sep.CapsValid);
    caps[10] = 8; caps[3] = 7; // codec TLV consumes next category byte
    CHECK(!AvdtpParseServiceCapabilities(caps, sizeof(caps), &sep));
}

static void TestMalformedPackets()
{
    ULONG random = 0x504e4132;
    auto next = [&]() { random ^= random << 13; random ^= random >> 17; random ^= random << 5; return random; };
    AVDTP_REASSEMBLY r = {};
    for (unsigned i = 0; i < 50000; ++i) {
        ULONG size = next() % 600;
        std::vector<UCHAR> bytes(size ? size : 1);
        for (ULONG j = 0; j < size; ++j) bytes[j] = (UCHAR)next();
        A2DP_CODEC_CONFIG cfg;
        A2DP_CODEC_CAPS caps;
        for (UCHAR type : {UCHAR(A2DP_CODEC_SBC), UCHAR(A2DP_CODEC_MPEG24_AAC), UCHAR(A2DP_CODEC_VENDOR)}) {
            A2dpParseCaps(type, bytes.data(), size, &caps, NULL, NULL);
            if (A2dpParseConfig(type, bytes.data(), size, &cfg)) {
                CHECK(cfg.SampleRate && cfg.Channels && cfg.IesLength <= sizeof(cfg.Ies));
            }
        }
        USHORT seq; ULONG timestamp, header;
        if (RtpParseHeader(bytes.data(), size, &seq, &timestamp, &header)) CHECK(header <= size);
        AVDTP_REMOTE_SEP sep = {};
        AvdtpParseServiceCapabilities(bytes.data(), size, &sep);
        if (AvdtpReassemble(&r, bytes.data(), size) == AvdtpComplete) CHECK(r.Length <= sizeof(r.Buffer) && !r.Active);
    }
    printf("Exercised 50,000 deterministic malformed packet inputs\n");
}

static void TestAudioPolicy()
{
    CHECK(!PnAdapterNeedsConservativeProfile(FALSE, 3));
    CHECK(!PnAdapterNeedsConservativeProfile(TRUE, 0));
    CHECK(PnAdapterNeedsConservativeProfile(TRUE, 3));
    CHECK(!PnAdapterNeedsConservativeProfile(TRUE, 3 | (1ull << 25)));
    CHECK(!PnAdapterNeedsConservativeProfile(TRUE, 3 | (1ull << 26)));
    CHECK(PnQualityBoundBitrate(96000, 128000, 320000) == 128000);
    CHECK(PnQualityBoundBitrate(96000, 500000, 192000) == 192000);
    PN_QUALITY_WINDOW window = {};
    CHECK(PnReconnectDelayMs(10, 1000) == 30000); // receiver can return after the initial retries
    CHECK(PnReconnectDelayMs(0, 1000) == 1500);
    CHECK(PnReconnectDelayMs(0xffffffffu, 0xffffffffu) == 30000);
    CHECK(PnReconnectDelayMs(9, 0xffffffffu) == 30000);
    PnHostRetry retry = {};
    for (unsigned i = 0; i < 50; ++i) { PnHostRetryFailed(&retry, 1000); CHECK(retry.next > 1000 && retry.next <= 31000); }
    retry.started = 1000; PnHostRetryHealthy(&retry, 2000); CHECK(retry.failures == 5);
    PnHostRetryHealthy(&retry, 31000); CHECK(retry.failures == 0);
    {
        PN_QUALITY_WINDOW recovery = {};
        ULONG level = 7;
        for (ULONG ms = 0; ms < 65000; ms += 250) {
            int change = PnQualityObserve(&recovery, 250, TRUE, FALSE, TRUE, level, 24);
            if (change < 0) --level;
        }
        CHECK(level == 0); // continued clean traffic should recover seven steps in 65 s
        recovery = {};
        for (ULONG ms = 0; ms < 30000; ms += 250) PnQualityObserve(&recovery, 250, TRUE, FALSE, TRUE, 2, 24);
        CHECK(PnQualityRecoveryTarget(&recovery) == 10000);
        int last = 0;
        for (ULONG ms = 0; ms < 2000; ms += 250) last = PnQualityObserve(&recovery, 250, TRUE, TRUE, TRUE, 1, 24);
        CHECK(last == 1 && PnQualityRecoveryTarget(&recovery) == 60000);
        for (ULONG ms = 0; ms < 59750; ms += 250) CHECK(PnQualityObserve(&recovery, 250, TRUE, FALSE, TRUE, 2, 24) == 0);
        CHECK(PnQualityObserve(&recovery, 250, TRUE, FALSE, TRUE, 2, 24) == -1);
        PnQualityObserve(&recovery, 250, FALSE, FALSE, TRUE, 1, 24);
        CHECK(PnQualityRecoveryTarget(&recovery) == 30000); // idle resets fast recovery
    }
    for (unsigned i = 0; i < 240; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, i % 8 == 0, TRUE, 0, 3) == 0); // transient spikes
    for (unsigned i = 0; i < 3; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, TRUE, 0, 3) == 0);
    CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, TRUE, 0, 3) == 1); // sustained congestion
    for (unsigned i = 0; i < 7; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, TRUE, 1, 3) == 0);
    CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, TRUE, 1, 3) == 1); // 2 s cooldown
    for (unsigned i = 0; i < 240; ++i) CHECK(PnQualityObserve(&window, 250, FALSE, FALSE, TRUE, 2, 3) == 0); // silence earns no recovery
    for (unsigned i = 0; i < 119; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, FALSE, TRUE, 2, 3) == 0);
    CHECK(PnQualityObserve(&window, 250, TRUE, FALSE, TRUE, 2, 3) == -1);
    window = {0, 29750, 30000};
    CHECK(PnQualityObserve(&window, 5000, TRUE, FALSE, TRUE, 1, 3) == 0 && window.GoodMs == 250); // scheduling gap
    for (unsigned i = 0; i < 200; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, TRUE, 3, 3) == 0); // floor
    for (unsigned i = 0; i < 200; ++i) CHECK(PnQualityObserve(&window, 250, TRUE, TRUE, FALSE, 0, 3) == 0); // disabled
    CHECK(!PnEndpointIsHeadphones(0x240414)); // Sony loudspeaker
    CHECK(!PnEndpointIsHeadphones(0x240428)); // hi-fi audio receiver
    CHECK(!PnEndpointIsHeadphones(0));
    CHECK(PnEndpointIsHeadphones(0x240404));
    CHECK(PnEndpointIsHeadphones(0x240408));
    CHECK(PnEndpointIsHeadphones(0x240418));
    CHECK(!PnEndpointIsHeadphones(0x240518)); // another major device class
    CHECK(PnMediaQueueHasCapacity(3, 1, 4));
    CHECK(!PnMediaQueueHasCapacity(4, 1, 4));
    CHECK(PnMediaQueueHasCapacity(2, 2, 4)); // AAC fragments require all slots
    CHECK(!PnMediaQueueHasCapacity(3, 2, 4));
    CHECK(!PnMediaQueueHasCapacity(0, 0, 4));
    CHECK(!PnMediaQueueHasCapacity(0xffffffff, 1, 4));
    CHECK(!PnMediaQueueHasCapacity(1, 0xffffffff, 4));
    CHECK(PnLdacAbrQuality(0, 0) == 0 && PnLdacAbrQuality(0, 1) == 1 && PnLdacAbrQuality(0, 2) == 2);
    CHECK(PnLdacAbrQuality(1, 0) == 1 && PnLdacAbrQuality(1, 1) == 2); // SQ must not upgrade on congestion
    CHECK(PnLdacAbrQuality(2, 0) == 2 && PnLdacAbrQuality(2, 1) == 2);
    CHECK(PnLdacAbrQuality(0, 0xffffffff) == 2);
    /* KSPROPSETID_BtAudio one-shots: Windows' Connect / Disconnect send GET with no value */
    CHECK(PnBtAudioOneShotVerbAccepted(PN_KS_VERB_GET));
    CHECK(PnBtAudioOneShotVerbAccepted(PN_KS_VERB_GET | 0x10000000UL));   /* KSPROPERTY_TYPE_TOPOLOGY */
    CHECK(PnBtAudioOneShotVerbAccepted(PN_KS_VERB_SET));                  /* earlier callers */
    CHECK(!PnBtAudioOneShotVerbAccepted(PN_KS_VERB_BASICSUPPORT));
    CHECK(!PnBtAudioOneShotVerbAccepted(0));
    CHECK(!PnBtAudioOneShotVerbAccepted(0x00000100UL));                   /* KSPROPERTY_TYPE_SETSUPPORT */
    static_assert((PN_BTAUDIO_ONESHOT_ACCESS & PN_KS_VERB_GET) && (PN_BTAUDIO_ONESHOT_ACCESS & PN_KS_VERB_SET), "basic support advertises GET and SET");
}

static void TestQualityCodecLimits()
{
    const ULONG codecs[] = {PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_LDAC};
    for (ULONG codec : codecs) for (ULONG rate : {44100u, 48000u, 96000u}) {
        UCHAR ie[32], type;
        A2DP_CODEC_CAPS caps = {}; A2DP_CODEC_CONFIG cfg = {}, reduced = {};
        PNA2DP_CONFIG pref = {}; pref.Sbc.MaxBitpool = 53; pref.Aac.BitrateBps = 320000;
        ULONG n = A2dpBuildLocalCaps(codec, ie, sizeof(ie), &type);
        CHECK(A2dpParseCaps(type, ie, n, &caps, NULL, NULL));
        CHECK(A2dpSelectConfig(&caps, &pref, rate, 24, &cfg));
        for (ULONG floor : {0u, 128000u, 200000u, 700000u, 2000000u}) {
            ULONG previous = cfg.BitrateBps;
            for (ULONG level = 0; level <= 24; ++level) {
                ULONG bitrate = A2dpQualityAtLevel(&cfg, level, floor, &reduced);
                CHECK(bitrate <= cfg.BitrateBps && bitrate <= previous);
                CHECK(bitrate >= (floor < cfg.BitrateBps ? floor : cfg.BitrateBps));
                CHECK(reduced.SampleRate == cfg.SampleRate && reduced.Channels == cfg.Channels);
                if (codec == PNA2DP_CODEC_SBC) CHECK(reduced.SbcBitpool >= cfg.SbcMinBitpool && reduced.SbcBitpool <= cfg.SbcMaxBitpool);
                previous = bitrate;
            }
        }
        A2DP_CODEC_CONFIG basic = cfg, parsed = {};
        BOOLEAN fits = A2dpConstrainBasicRadio(&basic);
        if (codec == PNA2DP_CODEC_APTX || codec == PNA2DP_CODEC_APTX_HD || codec == PNA2DP_CODEC_APTX_LL) CHECK(!fits);
        else {
            CHECK(fits && basic.BitrateBps <= 350000);
            CHECK(A2dpParseConfig(basic.MediaCodecType, basic.Ies, basic.IesLength, &parsed));
            CHECK(parsed.SampleRate == cfg.SampleRate);
        }
    }
}

#include "core_test_P.inl"
#include "core_test_R.inl"
#include "core_test_T.inl"
#include "core_test_G.inl"

int main()
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* a crash keeps the output before it */
    TestCodecNegotiation(); TestRtp(); TestPcm(); TestAvdtp(); TestAudioPolicy(); TestQualityCodecLimits();
    TestMalformedPackets();
    TestLaneP();
    TestLaneR();
    TestControlAccess();
    TestLaneW();
    TestLaneT();
    TestGlue();
    printf("%s (%u checks, %u failures)\n", failures ? "FAILED" : "PASSED", checks, failures);
    return failures ? 1 : 0;
}
