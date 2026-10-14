/*
 * pnctl_test.cpp - self test of the user-mode helper library used by the control panel and
 * the installer (tools/common). Runs without the driver; the device-enumeration checks only
 * require that the calls succeed on any machine.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include "pnctl.h"
#include "pndrv.h"
#include "pnretry.h"
#include "config_policy.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* the driver's own validation (regcfg.cpp RegCfgValidate = config_policy.h PnConfigValidate):
 * true when the driver would store the config unchanged */
static bool DriverKeepsUnchanged(const PNA2DP_CONFIG &in)
{
    PNA2DP_CONFIG c = in;
    return PnConfigValidate(&c) && memcmp(&c, &in, sizeof(c)) == 0;
}

static void TestConfigText()
{
    PNA2DP_CONFIG a;
    pn::ConfigDefaults(a);
    a.TargetLatencyMs = 90; a.MaxLatencyMs = 333; a.AbrFloorBps = 200000; a.VolumeDb16 = -6 * 65536 - 32768; a.ReconnectDelayMs = 4321;
    a.Sbc.MaxBitpool = 47; a.Sbc.BlockMask = PNA2DP_SBC_BLOCKS_16; a.Ldac.FormatMask = PNA2DP_FMT_S24; a.Aac.RateMask = PNA2DP_RATE_48000;
    a.Flags |= PNA2DP_CFG_AUTO_START_ON_CONNECT;
    a.Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ;   /* the default is HQ since 0.1.0.22; the edit below raises it */
    std::wstring t = pn::ConfigToText(a);
    PNA2DP_CONFIG b = {};
    CHECK(pn::ConfigFromText(t, b), "round trip parse");
    CHECK(memcmp(&a, &b, sizeof(a)) == 0, "round trip restores every byte");

    /* an edited key wins over the blob */
    std::wstring edited = t;
    size_t p = edited.find(L"latency=90");
    CHECK(p != std::wstring::npos, "export contains latency=90");
    if (p != std::wstring::npos) edited.replace(p, 10, L"latency=150");
    size_t q = edited.find(L"ldac.quality=sq");
    if (q != std::wstring::npos) edited.replace(q, 15, L"ldac.quality=HQ ");
    CHECK(pn::ConfigFromText(edited, b), "edited parse");
    CHECK(b.TargetLatencyMs == 150, "edited latency applied (got %u)", b.TargetLatencyMs);
    CHECK(b.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ, "case-insensitive, trimmed ldac.quality (got %u)", b.Ldac.Eqmid);
    CHECK(b.AbrFloorBps == 200000 && b.VolumeDb16 == a.VolumeDb16 && b.Sbc.BlockMask == PNA2DP_SBC_BLOCKS_16, "blob-only fields kept under edits");

    /* not a configuration */
    CHECK(!pn::ConfigFromText(L"name=value\r\nfoo=bar\r\n", b), "random key=value text is rejected");
    CHECK(!pn::ConfigFromText(L"", b), "empty text is rejected");
    /* key-only file over defaults, bad values ignored */
    CHECK(pn::ConfigFromText(L"latency=abc\r\nsbc.bitpool=300\r\nenable=ldac\r\n", b), "key-only parse");
    CHECK(b.TargetLatencyMs == 60, "non-numeric latency keeps default (got %u)", b.TargetLatencyMs);
    CHECK(b.Sbc.MaxBitpool == 0, "out-of-range bitpool keeps default (got %u)", b.Sbc.MaxBitpool);
    CHECK((b.EnabledCodecMask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC)) != 0, "SBC always enabled");
    CHECK(pn::ConfigFromText(L"# example blob=ignored\n" + t, b) && memcmp(&a, &b, sizeof(a)) == 0,
          "a comment mentioning blob must not hide the actual saved configuration");
    std::wstring malformed = t;
    size_t blob = malformed.find(L"blob=");
    malformed[blob + 5 + 2 * offsetof(PNA2DP_CONFIG, Reserved)] = L'z';
    b = a;
    CHECK(!pn::ConfigFromText(malformed, b) && memcmp(&a, &b, sizeof(a)) == 0,
          "invalid hex in the saved blob rejects the import without changing the destination");
    CHECK(!pn::ConfigFromText(L"blob=00\nlatency=100\n", b), "truncated blob cannot silently fall back to defaults");
    CHECK(pn::ConfigFromText(L"latency=-1\naac.bitrate=4294967296\n", b) && b.TargetLatencyMs == 60 && b.Aac.BitrateBps == 320000,
          "negative and overflowing numeric preferences are ignored rather than wrapped");
}

/* 0.1.0.22 #5: latency profiles in the text format and the helpers */
static void TestLatencyProfileText()
{
    const struct { ULONG profile; const wchar_t *key, *name; ULONG target, max; } k[] = {
        { PN_LATENCY_PROFILE_LOW, L"low", L"Low latency", 30, 120 },
        { PN_LATENCY_PROFILE_BALANCED, L"balanced", L"Balanced", 60, 250 },
        { PN_LATENCY_PROFILE_MUSIC, L"music", L"Music", 150, 450 },
    };
    for (const auto &e : k) {
        PNA2DP_CONFIG c;
        pn::ConfigDefaults(c);
        pn::ApplyLatencyProfile(c, e.profile);
        CHECK(c.TargetLatencyMs == e.target && c.MaxLatencyMs == e.max && pn::LatencyProfileOf(c) == e.profile, "profile %ls values", e.key);
        CHECK(!wcscmp(pn::LatencyProfileKey(e.profile), e.key) && !wcscmp(pn::LatencyProfileName(e.profile), e.name), "profile %ls names", e.key);
        ULONG back = 99;
        CHECK(pn::LatencyProfileFromKey(e.key, back) && back == e.profile, "profile %ls key parses", e.key);
        CHECK(DriverKeepsUnchanged(c), "profile %ls is accepted unchanged by the driver", e.key);
        std::wstring t = pn::ConfigToText(c);
        CHECK(t.find(std::wstring(L"latency.profile=") + e.key + L"\r\n") != std::wstring::npos, "export names profile %ls", e.key);
        CHECK(t.find(L"\nlatency=") == std::wstring::npos && t.find(L"maxlatency=") == std::wstring::npos, "a named profile exports no latency= lines");
        PNA2DP_CONFIG r = {};
        CHECK(pn::ConfigFromText(t, r) && !memcmp(&r, &c, sizeof(r)), "profile %ls round trip", e.key);
    }
    ULONG p = 0;
    CHECK(pn::LatencyProfileFromKey(L"LowLatency", p) && p == PN_LATENCY_PROFILE_LOW, "lowlatency alias, case-insensitive");
    CHECK(pn::LatencyProfileFromKey(L"custom", p) && p == PN_LATENCY_PROFILE_CUSTOM && !pn::LatencyProfileFromKey(L"loud", p), "custom and unknown keys");
    PNA2DP_CONFIG c;
    pn::ConfigDefaults(c);
    c.TargetLatencyMs = 90; c.MaxLatencyMs = 333;
    CHECK(pn::LatencyProfileOf(c) == PN_LATENCY_PROFILE_CUSTOM, "90/333 is custom");
    pn::ApplyLatencyProfile(c, PN_LATENCY_PROFILE_CUSTOM);
    CHECK(c.TargetLatencyMs == 90 && c.MaxLatencyMs == 333, "applying custom changes nothing");
    std::wstring t = pn::ConfigToText(c);
    CHECK(t.find(L"latency.profile=custom\r\nlatency=90\r\nmaxlatency=333\r\n") != std::wstring::npos, "custom exports both values");
    /* key-only files */
    PNA2DP_CONFIG r;
    CHECK(pn::ConfigFromText(L"latency.profile=music\n", r) && r.TargetLatencyMs == 150 && r.MaxLatencyMs == 450, "latency.profile=music alone");
    CHECK(pn::ConfigFromText(L"latency.profile=MUSIC\nlatency=100\n", r) && r.TargetLatencyMs == 100 && r.MaxLatencyMs == 450, "later lines win");
    CHECK(pn::ConfigFromText(L"latency=100\nlatency.profile=custom\n", r) && r.TargetLatencyMs == 100 && r.MaxLatencyMs == 250, "custom is a no-op");
    CHECK(pn::ConfigFromText(L"latency.profile=loud\nlatency=100\n", r) && r.TargetLatencyMs == 100, "unknown profile names are ignored");
    CHECK(!pn::ConfigFromText(L"latency.profile=loud\n", r), "an unknown profile alone is not a configuration");
    /* an edited profile line in an export wins over the blob */
    PNA2DP_CONFIG m;
    pn::ConfigDefaults(m);
    m.ReconnectDelayMs = 4321;
    t = pn::ConfigToText(m);
    size_t at = t.find(L"latency.profile=balanced");
    CHECK(at != std::wstring::npos, "defaults export the Balanced profile");
    if (at != std::wstring::npos) t.replace(at, 24, L"latency.profile=music");
    CHECK(pn::ConfigFromText(t, r) && r.TargetLatencyMs == 150 && r.MaxLatencyMs == 450 && r.ReconnectDelayMs == 4321, "edited profile applies over the blob");
    pn::Preset preset;
    CHECK(pn::PresetFromKey(L"best", preset) && preset == pn::Preset::BestQuality && pn::PresetFromKey(L"defaults", preset) && preset == pn::Preset::Balanced &&
          pn::PresetFromKey(L"LowLatency", preset) && preset == pn::Preset::LowLatency && pn::PresetFromKey(L"compat", preset) && preset == pn::Preset::Compatibility &&
          pn::PresetFromKey(L"sbcxq", preset) && preset == pn::Preset::SbcXq && !pn::PresetFromKey(L"loudest", preset), "preset keys");
    /* review #5: the latency profile words are not presets: "preset=balanced" / "preset=low" would
     * replace the whole configuration (codec order, SBC XQ / dual channel, auto start) */
    CHECK(!pn::PresetFromKey(L"balanced", preset) && !pn::PresetFromKey(L"Low", preset) && !pn::PresetFromKey(L"music", preset) &&
          pn::PresetFromKey(L"lowlatency", preset) && preset == pn::Preset::LowLatency, "latency profile words are not preset keys");
}

static void TestConfigFiles()
{
    wchar_t root[MAX_PATH]{}, temporary[MAX_PATH]{};
    CHECK(GetTempPathW(_countof(root), root) && GetTempFileNameW(root, L"pnt", 0, temporary), "create unique config test directory");
    if (!temporary[0]) return;
    DeleteFileW(temporary);
    CHECK(CreateDirectoryW(temporary, nullptr), "create test directory");
    std::wstring path = std::wstring(temporary) + L"\\settings-\u0e44\u0e17\u0e22.txt";
    PNA2DP_CONFIG a{}, b{};
    pn::ConfigDefaults(a); a.TargetLatencyMs = 90; a.ReconnectDelayMs = 4321;
    CHECK(pn::WriteConfigFile(path, a) == ERROR_SUCCESS, "export Unicode path");
    CHECK(pn::ReadConfigFile(path, b) == ERROR_SUCCESS && !memcmp(&a, &b, sizeof(a)), "file round trip preserves all fields");
    HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(locked != INVALID_HANDLE_VALUE, "lock original against replacement");
    PNA2DP_CONFIG changed = a; changed.TargetLatencyMs = 150;
    CHECK(pn::WriteConfigFile(path, changed) != ERROR_SUCCESS, "locked destination reports export failure");
    if (locked != INVALID_HANDLE_VALUE) CloseHandle(locked);
    CHECK(pn::ReadConfigFile(path, b) == ERROR_SUCCESS && !memcmp(&a, &b, sizeof(a)), "failed replacement leaves original export intact");
    CHECK(pn::WriteConfigFile(path, changed) == ERROR_SUCCESS && pn::ReadConfigFile(path, b) == ERROR_SUCCESS && !memcmp(&changed, &b, sizeof(b)), "successful replacement updates complete export");
    auto raw = [&](const std::string &bytes) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written = 0;
        CHECK(f != INVALID_HANDLE_VALUE && WriteFile(f, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size(), "write config fixture");
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    };
    raw("\xef\xbb\xbflatency=100\n");
    CHECK(pn::ReadConfigFile(path, b) == ERROR_SUCCESS && b.TargetLatencyMs == 100, "UTF-8 BOM accepted");
    for (const auto &bad : {std::string(), std::string("latency=100\n\xff"), std::string("latency=100\0junk", 16), std::string("blob=00\nlatency=100\n")}) {
        raw(bad); b = a;
        CHECK(pn::ReadConfigFile(path, b) != ERROR_SUCCESS && !memcmp(&a, &b, sizeof(a)), "invalid import preserves destination");
    }
    HANDLE large = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    LARGE_INTEGER size{}; size.QuadPart = 1024 * 1024 + 1;
    CHECK(large != INVALID_HANDLE_VALUE && SetFilePointerEx(large, size, nullptr, FILE_BEGIN) && SetEndOfFile(large), "create oversized fixture");
    if (large != INVALID_HANDLE_VALUE) CloseHandle(large);
    b = a;
    CHECK(pn::ReadConfigFile(path, b) == ERROR_FILE_TOO_LARGE && !memcmp(&a, &b, sizeof(a)), "oversized import rejected before allocation");
    CHECK(DeleteFileW(path.c_str()), "remove fixture");
    CHECK(pn::ReadConfigFile(path, b) == ERROR_FILE_NOT_FOUND && !memcmp(&a, &b, sizeof(a)), "missing import preserves destination");
    CHECK(RemoveDirectoryW(temporary), "no temporary export files left behind");
}

static void TestEventRetry()
{
    pn::EventRetryLimiter retry;
    CHECK(!retry.ShouldPause(L"device-a", 0), "first failure recovers even at uptime zero");
    CHECK(retry.ShouldPause(L"DEVICE-A", 1000), "reopened instance must pause repeated failures");
    CHECK(!retry.ShouldPause(L"device-b", 1100), "another headset gets its own recovery");
    CHECK(!retry.ShouldPause(L"device-b", 11100), "recovery permitted after backoff");
    CHECK(retry.ShouldPause(L"device-b", 12000), "next immediate failure pauses again");
    retry.Reset();
    CHECK(!retry.ShouldPause(L"device-b", 12001), "explicit Refresh permits retry");
    CHECK(!retry.ShouldPause(L"device-a", 0xffffffffull + 500) && retry.ShouldPause(L"device-a", 0xffffffffull + 1500), "long system uptime keeps backoff intact");
}

/* 0.1.0.22 (#2/#5): every codec starts at its best level; presets match the new defaults */
static void TestBestQualityDefaults()
{
    PNA2DP_CONFIG d;
    pn::ConfigDefaults(d);
    CHECK(d.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ && d.Ldac.AbrEnable && d.Ldac.FormatMask == 0, "defaults: LDAC HQ ceiling with ABR, automatic format");
    CHECK(d.Aac.BitrateBps == 320000 && d.Aac.Vbr == 0, "defaults: AAC at the sink's maximum up to 320 kbps, CBR");
    CHECK(d.Sbc.MaxBitpool == 0 && d.Sbc.MinBitpool == 2 && !d.Sbc.AllowBitpoolOverride && !(d.Flags & (PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_SBC_XQ)),
          "defaults: SBC at the receiver's maximum bitpool, channel-mode default unchanged");
    CHECK(d.CodecPriority[0] == PNA2DP_CODEC_LDAC && d.CodecPriority[1] == PNA2DP_CODEC_APTX_HD && d.CodecPriority[3] == PNA2DP_CODEC_APTX,
          "defaults: aptX HD before aptX");
    CHECK(d.TargetLatencyMs == 60 && d.MaxLatencyMs == 250 && (d.Flags & PNA2DP_CFG_ABR_ENABLE) && (d.Flags & PNA2DP_CFG_AUTO_QUALITY),
          "defaults: Balanced buffering, ABR as the safety net");
    CHECK(DriverKeepsUnchanged(d), "the driver keeps the defaults unchanged");
    /* automatic codecs restore exactly the defaults' codec sections and priority */
    PNA2DP_CONFIG a;
    pn::ConfigDefaults(a);
    a.Ldac.Eqmid = PNA2DP_LDAC_EQMID_MQ; a.Aac.BitrateBps = 128000; a.Aac.Vbr = 1; a.Sbc.MaxBitpool = 35;
    a.CodecPriority[0] = PNA2DP_CODEC_SBC; a.CodecPriority[5] = PNA2DP_CODEC_LDAC;
    pn::UseAutomaticCodecs(a);
    CHECK(!memcmp(a.CodecPriority, d.CodecPriority, sizeof(a.CodecPriority)) && a.EnabledCodecMask == d.EnabledCodecMask &&
          !memcmp(&a.Sbc, &d.Sbc, sizeof(a.Sbc)) && !memcmp(&a.Aac, &d.Aac, sizeof(a.Aac)) && !memcmp(&a.Aptx, &d.Aptx, sizeof(a.Aptx)) &&
          !memcmp(&a.AptxHd, &d.AptxHd, sizeof(a.AptxHd)) && !memcmp(&a.AptxLl, &d.AptxLl, sizeof(a.AptxLl)) && !memcmp(&a.Ldac, &d.Ldac, sizeof(a.Ldac)),
          "automatic codec sections equal the defaults");
    /* Best quality: Music buffering, the receiver's own SBC maximum (53 lowered receivers that advertise more) */
    PNA2DP_CONFIG b;
    pn::ConfigDefaults(b);
    b.Sbc.MaxBitpool = 40;
    pn::ApplyPreset(b, pn::Preset::BestQuality);
    CHECK(b.TargetLatencyMs == 150 && b.MaxLatencyMs == 450, "best quality uses the Music profile (got %u/%u)", b.TargetLatencyMs, b.MaxLatencyMs);
    CHECK(b.Sbc.MaxBitpool == 0 && !b.Sbc.AllowBitpoolOverride, "best quality keeps the receiver's maximum SBC bitpool (got %u)", b.Sbc.MaxBitpool);
    CHECK(b.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ && b.Ldac.FormatMask == PNA2DP_FMT_S24 && b.Aac.BitrateBps == 320000 && !b.Aac.Vbr &&
          (b.Flags & PNA2DP_CFG_ABR_ENABLE) && !(b.Flags & (PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_SBC_XQ)), "best quality codec settings");
    CHECK(b.CodecPriority[0] == PNA2DP_CODEC_LDAC && b.CodecPriority[1] == PNA2DP_CODEC_APTX_HD && b.CodecPriority[2] == PNA2DP_CODEC_AAC &&
          b.CodecPriority[3] == PNA2DP_CODEC_APTX && b.CodecPriority[4] == PNA2DP_CODEC_SBC && !b.CodecPriority[5], "best quality priority");
    /* Balanced is the full driver default (volume kept) */
    PNA2DP_CONFIG bal;
    pn::ConfigDefaults(bal);
    bal.TargetLatencyMs = 150; bal.MaxLatencyMs = 450; bal.Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ; bal.VolumeDb16 = -3 * 65536;
    pn::ApplyPreset(bal, pn::Preset::Balanced);
    PNA2DP_CONFIG expect = d; expect.VolumeDb16 = -3 * 65536;
    CHECK(!memcmp(&bal, &expect, sizeof(bal)), "the defaults preset equals the driver defaults, keeping the volume");
    PNA2DP_CONFIG low;
    pn::ConfigDefaults(low);
    pn::ApplyPreset(low, pn::Preset::LowLatency);
    CHECK(low.TargetLatencyMs == 30 && low.MaxLatencyMs == 120, "low latency preset uses the Low latency profile");
    for (pn::Preset p : { pn::Preset::Compatibility, pn::Preset::SbcXq }) {
        PNA2DP_CONFIG c;
        pn::ConfigDefaults(c);
        c.TargetLatencyMs = 150; c.MaxLatencyMs = 450;
        pn::ApplyPreset(c, p);
        CHECK(c.TargetLatencyMs == 60 && c.MaxLatencyMs == 250, "preset %d uses the default (Balanced) buffering", (int)p);
    }
    CHECK(std::wstring(pn::PresetName(pn::Preset::Balanced)).find(L"Driver defaults") == 0, "the Balanced preset is named after the driver defaults");
}

static void TestPresets()
{
    PNA2DP_CONFIG automatic;
    pn::ConfigDefaults(automatic);
    automatic.Sbc.AllowBitpoolOverride = 1; automatic.Sbc.MaxBitpool = 250;
    automatic.Sbc.RateMask = PNA2DP_SBC_FREQ_16000;
    automatic.Aac.RateMask = PNA2DP_RATE_96000;
    automatic.Flags |= PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_DUAL_CHANNEL_PREFER;
    automatic.VolumeDb16 = -12 * 65536; automatic.TargetLatencyMs = 80; automatic.ReconnectDelayMs = 4321;
    pn::UseAutomaticCodecs(automatic);
    CHECK((automatic.Flags & (PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_ABR_ENABLE)) == (PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_ABR_ENABLE), "automatic enables adapter policy and ABR");
    CHECK(automatic.Sbc.MaxBitpool == 0 && !automatic.Sbc.AllowBitpoolOverride && !automatic.Sbc.RateMask && !automatic.Aac.RateMask,
          "automatic removes manual codec restrictions and bitpool override");
    CHECK(!(automatic.Flags & (PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_DUAL_CHANNEL_PREFER)), "automatic permits kernel SBC fallback");
    CHECK(automatic.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ && automatic.Ldac.AbrEnable &&
          automatic.Aac.BitrateBps == 320000 && !automatic.Aac.Vbr, "automatic starts with high-quality codec settings");
    CHECK(automatic.VolumeDb16 == -12 * 65536 && automatic.TargetLatencyMs == 80 && automatic.ReconnectDelayMs == 4321,
          "automatic preserves volume and transport settings");
    for (int i = 0; i <= (int)pn::Preset::SbcXq; i++) {
        PNA2DP_CONFIG c;
        pn::ConfigDefaults(c);
        c.VolumeDb16 = -10 * 65536;
        c.ReconnectDelayMs = 5000;
        c.Flags |= PNA2DP_CFG_SOFTWARE_VOLUME;
        pn::ApplyPreset(c, (pn::Preset)i);
        CHECK(DriverKeepsUnchanged(c), "preset %d is accepted unchanged by the driver", i);
        CHECK(c.VolumeDb16 == -10 * 65536, "preset %d keeps the volume", i);
        CHECK((c.EnabledCodecMask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC)) != 0, "preset %d keeps SBC enabled", i);
        if ((pn::Preset)i != pn::Preset::Balanced) {
            CHECK(c.ReconnectDelayMs == 5000 && (c.Flags & PNA2DP_CFG_SOFTWARE_VOLUME), "preset %d leaves unrelated settings alone", i);
        }
        /* only the XQ preset turns SBC XQ on; every other preset turns it off again */
        PNA2DP_CONFIG xqFirst;
        pn::ConfigDefaults(xqFirst);
        pn::ApplyPreset(xqFirst, pn::Preset::SbcXq);
        pn::ApplyPreset(xqFirst, (pn::Preset)i);
        CHECK(((xqFirst.Flags & PNA2DP_CFG_SBC_XQ) != 0) == ((pn::Preset)i == pn::Preset::SbcXq), "preset %d sets SBC XQ only for the XQ preset", i);
    }
    PNA2DP_CONFIG x;
    pn::ConfigDefaults(x);
    CHECK(!(x.Flags & PNA2DP_CFG_SBC_XQ), "SBC XQ is off by default");
    pn::ApplyPreset(x, pn::Preset::SbcXq);
    /* the driver streams XQ at the bitpool plain dual channel gets, never a fixed 47 / 38 that
     * cost the Sony 600 or 492 instead of 672 kbps (known issue): the preset leaves the
     * receiver's maximum and never overrides it */
    CHECK((x.Flags & (PNA2DP_CFG_SBC_XQ | PNA2DP_CFG_DUAL_CHANNEL_PREFER)) == (PNA2DP_CFG_SBC_XQ | PNA2DP_CFG_DUAL_CHANNEL_PREFER) &&
          x.Sbc.MaxBitpool == 0 && !x.Sbc.AllowBitpoolOverride && x.Sbc.BlockMask == PNA2DP_SBC_BLOCKS_16 && x.Sbc.SubbandMask == PNA2DP_SBC_SUBBANDS_8,
          "SBC XQ preset asks the driver for XQ at the receiver's maximum bitpool and never overrides it");
    ULONG kbps = (pn::SbcBitrateBps(44100, 8, 16, PNA2DP_SBC_CH_DUAL, 53) + 500) / 1000;
    CHECK(kbps == 617, "SBC XQ at a receiver maximum of 53 is 617 kbps at 44.1 kHz (got %u)", kbps);
    kbps = (pn::SbcBitrateBps(48000, 8, 16, PNA2DP_SBC_CH_DUAL, 53) + 500) / 1000;
    CHECK(kbps == 672, "SBC XQ at a receiver maximum of 53 is 672 kbps at 48 kHz (got %u)", kbps);
    CHECK(13 + 3 * pn::SbcFrameBytes(8, 16, PNA2DP_SBC_CH_DUAL, 53) <= 1021 - 4 && 13 + 3 * pn::SbcFrameBytes(8, 16, PNA2DP_SBC_CH_DUAL, 53) <= 895,
          "three bitpool-53 dual-channel frames fit MTU 895 and one 3-DH5 baseband packet");
    PNA2DP_CONFIG back;
    CHECK(pn::ConfigFromText(pn::ConfigToText(x), back) && (back.Flags & PNA2DP_CFG_SBC_XQ), "SBC XQ survives the text round trip");
    CHECK(pn::ConfigFromText(pn::ConfigToText(x) + L"sbc.xq=0\r\n", back) && !(back.Flags & PNA2DP_CFG_SBC_XQ), "sbc.xq=0 turns it off");
    PNA2DP_CONFIG automatic2 = x;
    pn::UseAutomaticCodecs(automatic2);
    CHECK(!(automatic2.Flags & PNA2DP_CFG_SBC_XQ), "automatic codecs turn SBC XQ off");
}

static void TestSbcNumbers()
{
    CHECK((pn::SbcBitrateBps(44100, 8, 16, PNA2DP_SBC_CH_JOINT, 53) + 500) / 1000 == 328, "joint 53 @44.1 = 328 kbps");
    CHECK(pn::SbcFrameBytes(8, 16, PNA2DP_SBC_CH_JOINT, 53) == 119, "joint 53 frame = 119 bytes");
    CHECK(pn::SbcFrameBytes(8, 16, PNA2DP_SBC_CH_DUAL, 38) == 164, "dual 38 frame = 164 bytes");
    CHECK(pn::LdacBitrateBps(PNA2DP_LDAC_EQMID_HQ, 48000) == 990000 && pn::LdacBitrateBps(PNA2DP_LDAC_EQMID_MQ, 44100) == 303000, "LDAC ladder");
}

static void TestCodecActivation()
{
    PNA2DP_CONFIG original;
    pn::ConfigDefaults(original); pn::UseAutomaticCodecs(original);
    original.VolumeDb16 = -10 * 65536; original.TargetLatencyMs = 90;
    PNA2DP_CAPABILITIES caps = {}; caps.SepCount = 1;
    caps.CodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    caps.Seps[0].Codec = PNA2DP_CODEC_SBC; caps.Seps[0].TsepIsSink = 1;
    PNA2DP_VERSION v = {}; v.KernelCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    v.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    PNA2DP_CONFIG c = original;
    CHECK(!pn::PreferCodec(c, caps, v, PNA2DP_CODEC_LDAC) && !memcmp(&c, &original, sizeof(c)), "unsupported activation leaves all settings unchanged, even with a broad mask");
    CHECK(!pn::CanActivateCodec(caps, v, 0xffffffff) && !pn::CanActivateCodec(caps, v, 0), "invalid codec ids rejected");
    CHECK(pn::PreferCodec(c, caps, v, PNA2DP_CODEC_SBC) && c.CodecPriority[0] == PNA2DP_CODEC_SBC, "SBC activation moves supported codec first");
    CHECK(c.CodecPriority[1] == original.CodecPriority[0] && c.CodecPriority[5] == PNA2DP_CODEC_APTX_LL, "remaining priority order preserved");
    CHECK(c.VolumeDb16 == original.VolumeDb16 && c.TargetLatencyMs == original.TargetLatencyMs && !memcmp(&c.Ldac, &original.Ldac, sizeof(c.Ldac)), "activation preserves volume, buffers and codec settings");
    caps.Seps[1].Codec = PNA2DP_CODEC_LDAC; caps.Seps[1].TsepIsSink = 0; caps.SepCount = 2;
    CHECK(!pn::CanActivateCodec(caps, v, PNA2DP_CODEC_LDAC), "a source SEP does not advertise receiver support");
    caps.Seps[1].TsepIsSink = 1; caps.Seps[1].MediaType = 1;
    CHECK(!pn::CanActivateCodec(caps, v, PNA2DP_CODEC_LDAC), "a non-audio SEP cannot be activated");
    caps.Seps[1].MediaType = 0; v.HostCodecMask = 0;
    CHECK(!pn::CanActivateCodec(caps, v, PNA2DP_CODEC_LDAC), "missing encoder blocks activation");
    v.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    c.EnabledCodecMask &= ~PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC); c.Flags &= ~PNA2DP_CFG_APPLY_LIVE;
    CHECK(pn::PreferCodec(c, caps, v, PNA2DP_CODEC_LDAC) && (c.EnabledCodecMask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC)) &&
        (c.EnabledCodecMask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC)) && (c.Flags & PNA2DP_CFG_APPLY_LIVE), "activation enables selected codec, preserves fallback, applies live");
    CHECK(pn::CodecSettingsText(original, PNA2DP_CODEC_LDAC) == L"HQ ceiling; ABR on", "saved LDAC HQ is displayed, not a default preset");
    CHECK(pn::CodecSettingsText(original, PNA2DP_CODEC_AAC) == L"320 kbps; CBR", "saved AAC bitrate/mode displayed");
    CHECK(pn::CodecSettingsText(original, PNA2DP_CODEC_SBC).find(L"receiver max") != std::wstring::npos, "SBC auto limit is not replaced by a generic bitrate");
}

static void TestCodecPageDetails()
{
    PNA2DP_CAPABILITIES caps = {};
    PNA2DP_STATUS status = {};
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 0, "unknown SBC capability must not assume bitpool 53");
    CHECK(pn::CodecPageStatus(caps, status, PNA2DP_CODEC_AAC).find(L"unknown") != std::wstring::npos, "unknown support stays unknown");
    caps.SepCount = 2;
    for (ULONG i = 0; i < 2; ++i) {
        auto &s = caps.Seps[i]; s.Seid = (UCHAR)(i + 1); s.Codec = PNA2DP_CODEC_SBC; s.TsepIsSink = 1; s.CodecInfoLength = 4;
        s.CodecInfo[0] = s.CodecInfo[1] = 0xff; s.CodecInfo[2] = 2; s.CodecInfo[3] = (UCHAR)(i ? 53 : 35);
    }
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 53 && pn::SbcReceiverMaxBitpool(caps, 1) == 35, "selected SBC endpoint limit takes precedence");
    caps.Seps[1].TsepIsSink = 0;
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 35, "ignore source endpoint limit");
    caps.Seps[1].TsepIsSink = 1; caps.Seps[1].MediaType = 1;
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 35, "ignore non-audio endpoint limit");
    caps.Seps[1].MediaType = 0; caps.Seps[1].CodecInfo[3] = 251;
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 35, "ignore malformed bitpool limit");
    caps.Seps[1].CodecInfoLength = 3;
    CHECK(pn::SbcReceiverMaxBitpool(caps) == 35, "ignore truncated SBC IE");
    status.Codec = PNA2DP_CODEC_SBC; status.State = PNA2DP_STATE_STREAMING;
    status.BitrateBps = 357000; status.SampleRate = 48000; status.Channels = 2;
    auto text = pn::CodecPageStatus(caps, status, PNA2DP_CODEC_SBC);
    CHECK(text.find(L"Live: 357 kbps, 48000 Hz, 2 channels") != std::wstring::npos, "detail page uses live driver format and bitrate");
    text = pn::CodecPageStatus(caps, status, PNA2DP_CODEC_LDAC);
    CHECK(text.find(L"not supported") != std::wstring::npos && text.find(L"Not active") != std::wstring::npos && text.find(L"Current output: SBC") != std::wstring::npos,
        "saved LDAC settings are not presented as active on an SBC receiver");
    status.State = PNA2DP_STATE_IDLE;
    CHECK(pn::CodecPageStatus(caps, status, PNA2DP_CODEC_SBC).find(L"Live:") == std::wstring::npos, "stale disconnected status is not live");
}

/* 0.1.0.22 #1: the status shows the Windows rate the driver converts from */
static void TestRateConversionStatus()
{
    PNA2DP_STATUS s = {};
    s.SampleRate = 48000; s.Channels = 2; s.BitsPerSample = 24;
    CHECK(pn::StreamFormatText(s) == L"48000 Hz, 2 ch, 24-bit in", "native format: %ls", pn::StreamFormatText(s).c_str());
    s.EngineSampleRate = 48000;
    CHECK(pn::StreamFormatText(s) == L"48000 Hz, 2 ch, 24-bit in", "the engine at the codec rate: no conversion shown");
    s.EngineSampleRate = 44100;
    CHECK(pn::StreamFormatText(s) == L"48000 Hz (Windows 44100 Hz, converted), 2 ch, 24-bit in", "converted: %ls", pn::StreamFormatText(s).c_str());
    /* 0.1.0.36: the panel's Status page shows the Windows rate on its own row; the combined line
     * did not fit the value column and lost its end */
    CHECK(pn::StreamFormatText(s, false) == L"48000 Hz, 2 ch, 24-bit in", "format without the Windows rate: %ls", pn::StreamFormatText(s, false).c_str());
    CHECK(pn::EngineRateText(s) == L"44100 Hz, converted to 48000 Hz", "converted Windows rate: %ls", pn::EngineRateText(s).c_str());
    s.EngineSampleRate = 48000;
    CHECK(pn::EngineRateText(s) == L"48000 Hz, no conversion", "native Windows rate: %ls", pn::EngineRateText(s).c_str());
    s.EngineSampleRate = 0;
    CHECK(pn::EngineRateText(s) == L"-", "no Windows stream, or a driver before 0.1.0.22");
    s.EngineSampleRate = 44100; s.SampleRate = 0;
    CHECK(pn::EngineRateText(s) == L"44100 Hz", "Windows open before the stream is configured: %ls", pn::EngineRateText(s).c_str());
    s.SampleRate = 48000;
    CHECK(pn::StatusFlagsText(PNA2DP_STATUS_FLAG_RATE_CONVERTED) == L"rate converted", "flag text");
    CHECK(pn::StatusFlagsText(PNA2DP_STATUS_FLAG_RATE_CONVERTED | PNA2DP_STATUS_FLAG_HALF_RATE) == L"rate converted (2:1)", "2:1 text");
    CHECK(pn::StatusFlagsText(PNA2DP_STATUS_FLAG_HALF_RATE) == L"2:1 rate conversion", "a 0.1.0.21 driver's half-band flag");
    /* ABI 1: EngineSampleRate is a former reserved word, the structure keeps its size */
    static_assert(sizeof(PNA2DP_STATUS) == 224 && offsetof(PNA2DP_STATUS, EngineSampleRate) == 208 && sizeof(s.Reserved) == 12, "PNA2DP_STATUS layout");
}

static void TestErrorText()
{
    std::wstring s = pn::Win32ErrorText(0xE000022F);
    CHECK(s.find(L"catalog") != std::wstring::npos, "SetupAPI 0xE000022F has text: %ls", s.c_str());
    s = pn::Win32ErrorText(ERROR_ACCESS_DENIED);
    CHECK(s.find(L'\n') == std::wstring::npos && s.size() > 20, "system error text is one line: %ls", s.c_str());
    s = pn::NtStatusText(0xC0000022);
    CHECK(s.find(L'\n') == std::wstring::npos, "NTSTATUS text is one line: %ls", s.c_str());
}

static void TestAddressParsing()
{
    ULONGLONG a = 0;
    pn::ParseBtAddressFromInstanceId(L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}_LOCALMFG&000F\\7&1A2B3C4D&0&E8EC4A123456_C00000000", &a);
    CHECK(a == 0xE8EC4A123456ull, "address parsed");
    CHECK(pn::BtAddressText(a) == L"E8:EC:4A:12:34:56", "address text");
    pn::ParseBtAddressFromInstanceId(L"garbage", &a);
    CHECK(a == 0, "garbage gives 0");
}

static void TestDriverPackageSelection()
{
    CHECK(pn::CompareDriverVersions(L"0.1.0.16", L"0.1.0.9") > 0, "versions are numeric, not alphabetical");
    CHECK(pn::CompareDriverVersions(L"0.2.0.0", L"0.1.65535.65535") > 0, "version component precedence");
    CHECK(pn::CompareDriverVersions(L" 0.1.0.16 ", L"0.1.0.16") == 0, "INF version whitespace ignored");
    for (auto bad : { L"", L"garbage", L"0.1.0.9999999999999999", L"0.1.0.16junk", L"0.1.0", L"-1.1.0.0" })
        CHECK(pn::CompareDriverVersions(bad, L"0.1.0.0") < 0, "malformed version cannot outrank a valid package");
    std::vector<pn::StagedPackage> packages = {
        { L"oem75.inf", L"C:\\Windows\\INF\\oem75.inf", L"0.1.0.16", L"PN-A2DP Project" },
        { L"oem9.inf", L"C:\\Windows\\INF\\oem9.inf", L"0.1.0.0", L"PN-A2DP Project" },
        { L"oem99.inf", L"C:\\Windows\\INF\\oem99.inf", L"0.1.0.9", L"PN-A2DP Project" }
    };
    // Regression: enumeration returned oem9 last and silently reinstalled 0.1.0.0.
    for (int order = 0; order < 3; ++order) {
        pn::SortStagedPackages(packages);
        CHECK(packages.back().publishedName == L"oem75.inf", "automatic switch picks newest DriverVer regardless of OEM number");
        std::rotate(packages.begin(), packages.begin() + 1, packages.end());
    }
    auto live = pn::FindStagedPackages();
    for (size_t i = 1; i < live.size(); ++i)
        CHECK(pn::CompareDriverVersions(live[i-1].version, live[i].version) <= 0, "real driver store ordered by numeric version");
    if (!live.empty()) printf("  newest staged PN driver: %ls (%ls)\n", live.back().publishedName.c_str(), live.back().version.c_str());
}

static void TestMachine()
{
    auto nodes = pn::EnumerateA2dpSinkNodes();
    printf("  %zu A2DP sink node(s)\n", nodes.size());
    for (auto &n : nodes) {
        printf("    %ls  driver=%ls (%ls)  present=%d started=%d connected=%d\n", n.name.c_str(), n.driverInf.c_str(), n.driverProvider.c_str(), n.present, n.started, n.connected);
        CHECK(!n.started || n.present, "started implies present");
    }
    pn::SigningState s = pn::QuerySigningState();
    printf("  signing: queried=%d testSigning=%d secureBoot=%d hvci=%d elevated=%d\n",
           s.queried, s.testSigning, s.secureBoot, s.hvci, pn::IsElevated());
    CHECK(s.queried, "code integrity state queried");
    pn::Device d;
    CHECK(!d.Open(L"\\\\?\\bogus#path"), "bogus open fails");
    ULONG mask = 0; DWORD err = 0;
    CHECK(!d.WaitEvent(mask, 10, &err) && err == ERROR_INVALID_HANDLE, "WaitEvent on a closed device reports ERROR_INVALID_HANDLE through errOut");
    d.Cancel();
}

int main()
{
    TestConfigText();
    TestLatencyProfileText();
    TestConfigFiles();
    TestEventRetry();
    TestPresets();
    TestBestQualityDefaults();
    TestSbcNumbers();
    TestCodecActivation();
    TestCodecPageDetails();
    TestRateConversionStatus();
    TestErrorText();
    TestAddressParsing();
    TestDriverPackageSelection();
    TestMachine();
    printf(g_fail ? "FAILED (%d)\n" : "PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
