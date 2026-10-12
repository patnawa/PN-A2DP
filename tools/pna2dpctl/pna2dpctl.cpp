/*
 * pna2dpctl - command line control for the PN-A2DP driver.
 *
 *   pna2dpctl list                      enumerate A2DP devices served by pna2dp
 *   pna2dpctl status  [-d N]            live status (codec, bitrate, latency, flags)
 *   pna2dpctl caps    [-d N]            codecs and parameters the headset advertises
 *   pna2dpctl config  [-d N]            show configuration
 *   pna2dpctl set     [-d N] key=value  change configuration (applied live when possible)
 *   pna2dpctl watch   [-d N]            print status on every driver event
 *   pna2dpctl stats   [-d N]            transport statistics
 *   pna2dpctl reconnect [-d N]
 *
 * Keys for `set`: codecs=ldac,aptxhd,aac,aptx,aptxll,sbc (priority order)
 *                 enable=<codec>,...   disable=<codec>,...
 *                 sbc.bitpool=N        sbc.minbitpool=N   sbc.override=0|1   sbc.dual=0|1
 *                 sbc.rates=44100,48000  sbc.subbands=4|8 sbc.blocks=4|8|12|16 sbc.alloc=loudness|snr
 *                 aac.bitrate=N        aac.vbr=0|1
 *                 ldac.quality=hq|sq|mq ldac.abr=0|1  ldac.bits=16|24|32
 *                 abr=0|1  latency=N(ms)  maxlatency=N(ms)  allrates=0|1  swvolume=0|1  delayreport=0|1
 *                 suspendonsco=0|1  hostonly=0|1
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "pna2dp_public.h"
#include "../common/pnctl.h"
#include "../common/pnspatial.h"
#include "../common/pnaudio.h"
#include "../common/pndrv.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

static std::vector<std::wstring> EnumerateDevices()
{
    std::vector<std::wstring> out;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2) return out;
    std::vector<WCHAR> buf(len);
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return out;
    for (PWCHAR p = buf.data(); *p; p += wcslen(p) + 1) out.push_back(p);
    return out;
}

static HANDLE OpenDevice(int index)
{
    auto list = EnumerateDevices();
    if (list.empty()) { fprintf(stderr, "no PN-A2DP devices found (is the driver installed and the headset paired?)\n"); return INVALID_HANDLE_VALUE; }
    if (index < 0 || index >= (int)list.size()) { fprintf(stderr, "device index out of range (0..%zu)\n", list.size() - 1); return INVALID_HANDLE_VALUE; }
    HANDLE h = CreateFileW(list[index].c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) fprintf(stderr, "open failed: %lu\n", GetLastError());
    return h;
}

static BOOL Ioctl(HANDLE h, DWORD code, const void *in, DWORD inLen, void *out, DWORD outLen, DWORD *ret = NULL)
{
    DWORD r = 0;
    BOOL ok = DeviceIoControl(h, code, (LPVOID)in, inLen, out, outLen, &r, NULL);
    if (ret) *ret = r;
    if (!ok) fprintf(stderr, "ioctl 0x%08lx failed: %lu\n", code, GetLastError());
    return ok;
}

static const char *CodecName(ULONG c)
{
    switch (c) {
    case PNA2DP_CODEC_SBC: return "SBC"; case PNA2DP_CODEC_AAC: return "AAC"; case PNA2DP_CODEC_APTX: return "aptX";
    case PNA2DP_CODEC_APTX_HD: return "aptX HD"; case PNA2DP_CODEC_APTX_LL: return "aptX LL"; case PNA2DP_CODEC_LDAC: return "LDAC";
    case PNA2DP_CODEC_FASTSTREAM: return "FastStream"; case PNA2DP_CODEC_OPUS: return "Opus"; case PNA2DP_CODEC_LC3PLUS: return "LC3plus";
    default: return "none";
    }
}
static ULONG CodecFromName(const char *n)
{
    struct { const char *n; ULONG c; } t[] = { {"sbc",PNA2DP_CODEC_SBC},{"aac",PNA2DP_CODEC_AAC},{"aptx",PNA2DP_CODEC_APTX},{"aptxhd",PNA2DP_CODEC_APTX_HD},
        {"aptx-hd",PNA2DP_CODEC_APTX_HD},{"aptxll",PNA2DP_CODEC_APTX_LL},{"aptx-ll",PNA2DP_CODEC_APTX_LL},{"ldac",PNA2DP_CODEC_LDAC},{"faststream",PNA2DP_CODEC_FASTSTREAM},{"opus",PNA2DP_CODEC_OPUS} };
    for (auto &e : t) if (_stricmp(e.n, n) == 0) return e.c;
    return 0;
}
static const char *StateName(ULONG s)
{
    static const char *n[] = { "idle", "connecting", "discovering", "configured", "open", "streaming", "suspended", "closing", "error" };
    return s < 9 ? n[s] : "?";
}
static std::string RateMaskStr(ULONG m)
{
    static const struct { ULONG b; const char *s; } t[] = { {PNA2DP_RATE_16000,"16k"},{PNA2DP_RATE_32000,"32k"},{PNA2DP_RATE_44100,"44.1k"},{PNA2DP_RATE_48000,"48k"},{PNA2DP_RATE_88200,"88.2k"},{PNA2DP_RATE_96000,"96k"} };
    std::string r;
    for (auto &e : t) if (m & e.b) { if (!r.empty()) r += ","; r += e.s; }
    return r.empty() ? "-" : r;
}

static void PrintStatus(HANDLE h)
{
    PNA2DP_STATUS s = {};
    if (!Ioctl(h, IOCTL_PNA2DP_GET_STATUS, NULL, 0, &s, sizeof(s))) return;
    printf("device      : %ls (%012llX)\n", s.RemoteName, s.RemoteAddress);
    printf("volume mode : %s\n", (s.Flags & PNA2DP_STATUS_FLAG_REMOTE_VOLUME) ? "receiver absolute volume" : "software fallback");
    printf("state       : %s%s%s%s%s\n", StateName(s.State), (s.Flags & PNA2DP_STATUS_FLAG_SCO_ACTIVE) ? " [SCO active]" : "",
           (s.Flags & PNA2DP_STATUS_FLAG_AUDIO_ACTIVE) ? " [audio running]" : "", (s.Flags & PNA2DP_STATUS_FLAG_REMOTE_INITIATED) ? " [remote initiated]" : "",
           (s.Flags & PNA2DP_STATUS_FLAG_HOST_MISSING) ? " [codec host missing]" : "");
    printf("codec       : %s%s  %u Hz, %u ch, %u-bit in  (encoder: %s)\n", CodecName(s.Codec), (s.Flags & PNA2DP_STATUS_FLAG_ABR_ACTIVE) ? " (ABR reduced)" : "",
           s.SampleRate, s.Channels, s.BitsPerSample, s.EncoderOwner == 1 ? "kernel" : s.EncoderOwner == 2 ? "codec host" : "-");
    printf("bitrate     : %u kbps (max %u)\n", s.BitrateBps / 1000, s.MaxBitrateBps / 1000);
    printf("latency     : %u ms estimated, sink reports %u.%u ms%s\n", s.EstimatedLatencyUs / 1000, s.SinkReportedDelayUs / 1000, (s.SinkReportedDelayUs / 100) % 10,
           (s.Flags & PNA2DP_STATUS_FLAG_DELAY_REPORTING) ? "" : " (no delay reporting)");
    printf("media mtu   : %u   AVDTP %x.%x   remote SEID %u\n", s.MediaMtu, s.AvdtpVersion >> 8, s.AvdtpVersion & 0xFF, s.RemoteSeid);
    if (s.LastError) printf("last error  : 0x%08x\n", s.LastError);
}

static void PrintCaps(HANDLE h)
{
    PNA2DP_CAPABILITIES c = {};
    if (!Ioctl(h, IOCTL_PNA2DP_GET_CAPABILITIES, NULL, 0, &c, sizeof(c))) return;
    printf("AVDTP %x.%x (SDP), SupportedFeatures 0x%04x, %u stream endpoints\n", c.SdpProfileVersion >> 8, c.SdpProfileVersion & 0xFF, c.SdpSupportedFeatures, c.SepCount);
    for (ULONG i = 0; i < c.SepCount; i++) {
        PNA2DP_REMOTE_SEP &s = c.Seps[i];
        printf("  SEID %2u %s %-10s", s.Seid, s.TsepIsSink ? "SNK" : "SRC", CodecName(s.Codec));
        if (s.Codec == PNA2DP_CODEC_NONE) printf(" (vendor %08x/%04x)", s.VendorId, s.VendorCodecId);
        if (s.InUse) printf(" [in use]");
        if (s.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) printf(" [delay reporting]");
        if (s.ServiceCaps & PNA2DP_SVC_CONTENT_PROTECTION) printf(" [SCMS-T]");
        printf("  IE:");
        for (ULONG k = 0; k < s.CodecInfoLength; k++) printf(" %02x", s.CodecInfo[k]);
        if (s.Codec == PNA2DP_CODEC_SBC && s.CodecInfoLength >= 4) {
            printf("\n           rates %s%s%s%s  modes %s%s%s%s  bitpool %u..%u", (s.CodecInfo[0] & 0x80) ? "16k " : "", (s.CodecInfo[0] & 0x40) ? "32k " : "",
                   (s.CodecInfo[0] & 0x20) ? "44.1k " : "", (s.CodecInfo[0] & 0x10) ? "48k" : "", (s.CodecInfo[0] & 0x08) ? "mono " : "", (s.CodecInfo[0] & 0x04) ? "dual " : "",
                   (s.CodecInfo[0] & 0x02) ? "stereo " : "", (s.CodecInfo[0] & 0x01) ? "joint" : "", s.CodecInfo[2], s.CodecInfo[3]);
        }
        printf("\n");
    }
}

static void PrintConfig(const PNA2DP_CONFIG &c)
{
    printf("codec priority : ");
    for (int i = 0; i < 8 && c.CodecPriority[i]; i++) printf("%s%s", i ? " > " : "", CodecName(c.CodecPriority[i]));
    printf("\nenabled        :");
    for (ULONG k = 1; k < PNA2DP_CODEC_MAX; k++) if (c.EnabledCodecMask & PNA2DP_CODEC_BIT(k)) printf(" %s", CodecName(k));
    printf("\nflags          :%s%s%s%s%s%s%s%s%s\n", (c.Flags & PNA2DP_CFG_ABR_ENABLE) ? " abr" : "", (c.Flags & PNA2DP_CFG_APPLY_LIVE) ? " apply-live" : "",
           (c.Flags & PNA2DP_CFG_SUSPEND_ON_SCO) ? " suspend-on-sco" : "", (c.Flags & PNA2DP_CFG_AUTO_START_ON_CONNECT) ? " auto-start" : "",
           (c.Flags & PNA2DP_CFG_EXPOSE_ALL_RATES) ? " all-rates" : "", (c.Flags & PNA2DP_CFG_SOFTWARE_VOLUME) ? " sw-volume" : "",
           (c.Flags & PNA2DP_CFG_DELAY_REPORTING) ? " delay-reporting" : "", (c.Flags & PNA2DP_CFG_DUAL_CHANNEL_PREFER) ? " prefer-dual" : "",
           (c.Flags & PNA2DP_CFG_HOST_CODECS_ONLY) ? " host-only" : "");
    printf("auto quality   : %s\n", (c.Flags & PNA2DP_CFG_AUTO_QUALITY) ? "enabled" : "disabled");
    printf("latency        : target %u ms, max %u ms, ABR floor %u kbps\n", c.TargetLatencyMs, c.MaxLatencyMs, c.AbrFloorBps / 1000);
    printf("SBC            : bitpool %u..%u%s, rates %s, modes 0x%02x, blocks 0x%02x, subbands 0x%02x, alloc 0x%02x\n", c.Sbc.MinBitpool, c.Sbc.MaxBitpool,
           c.Sbc.AllowBitpoolOverride ? " (override allowed)" : "", c.Sbc.RateMask ? "custom" : "any", c.Sbc.ChannelModeMask, c.Sbc.BlockMask, c.Sbc.SubbandMask, c.Sbc.AllocMask);
    printf("AAC            : bitrate %u kbps%s, vbr %u, object %u\n", c.Aac.BitrateBps / 1000, c.Aac.BitrateBps ? "" : " (sink max)", c.Aac.Vbr, c.Aac.ObjectType);
    printf("LDAC           : quality %s, abr %u, rates %s, formats 0x%02x\n", c.Ldac.Eqmid == 0 ? "HQ (990k)" : c.Ldac.Eqmid == 1 ? "SQ (660k)" : c.Ldac.Eqmid == 2 ? "MQ (330k)" : "ABR",
           c.Ldac.AbrEnable, RateMaskStr(c.Ldac.RateMask).c_str(), c.Ldac.FormatMask);
    printf("volume         : %d dB (persisted)\n", c.VolumeDb16 / 65536);
}

static void PrintStats(HANDLE h)
{
    PNA2DP_STATS s = {};
    if (!Ioctl(h, IOCTL_PNA2DP_GET_STATS, NULL, 0, &s, sizeof(s))) return;
    printf("pcm frames consumed : %llu\n", s.PcmFramesConsumed);
    printf("transport waits     : %u   WaveRT late ticks %u, max %u us\n", s.TransportWaits, s.WaveLateTicks, s.WaveMaxTickUs);
    printf("packets sent        : %llu (%llu bytes, %llu codec frames)\n", s.PacketsSent, s.BytesSent, s.EncodedFrames);
    printf("packets dropped     : %llu   write failures %llu   latency trims %llu\n", s.PacketsDropped, s.WriteFailures, s.Underruns);
    printf("tx queue            : %u now, %u max   completion avg %u us max %u us\n", s.QueueDepthPackets, s.QueueDepthMaxPackets, s.TxCompletionAvgUs, s.TxCompletionMaxUs);
    printf("ABR                 : level %u, %u downgrades, %u upgrades\n", s.AbrLevel, s.AbrDowngrades, s.AbrUpgrades);
    printf("codec host          : round trip avg %u us, timeouts %u\n", s.HostRoundTripAvgUs, s.HostTimeouts);
}

static bool ParseRates(const char *v, ULONG *mask)
{
    *mask = 0;
    std::string s(v);
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(',', pos); if (e == std::string::npos) e = s.size();
        ULONG hz = (ULONG)atol(s.substr(pos, e - pos).c_str());
        switch (hz) {
        case 16000: *mask |= PNA2DP_RATE_16000; break; case 32000: *mask |= PNA2DP_RATE_32000; break; case 44100: *mask |= PNA2DP_RATE_44100; break;
        case 48000: *mask |= PNA2DP_RATE_48000; break; case 88200: *mask |= PNA2DP_RATE_88200; break; case 96000: *mask |= PNA2DP_RATE_96000; break;
        default: return false;
        }
        pos = e + 1;
    }
    return true;
}

static int DoSet(HANDLE h, int argc, char **argv, int start)
{
    PNA2DP_CONFIG c = {};
    if (!Ioctl(h, IOCTL_PNA2DP_GET_CONFIG, NULL, 0, &c, sizeof(c))) return 1;
    for (int i = start; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (!eq) { fprintf(stderr, "expected key=value: %s\n", argv[i]); return 1; }
        *eq = 0;
        const char *k = argv[i], *v = eq + 1;
        auto flag = [&](ULONG bit) { if (atoi(v)) c.Flags |= bit; else c.Flags &= ~bit; };
        if (!_stricmp(k, "codecs")) {
            memset(c.CodecPriority, 0, sizeof(c.CodecPriority));
            std::string s(v); size_t pos = 0; int n = 0;
            while (pos <= s.size() && n < 8) {
                size_t e = s.find(',', pos); if (e == std::string::npos) e = s.size();
                ULONG cc = CodecFromName(s.substr(pos, e - pos).c_str());
                if (!cc) { fprintf(stderr, "unknown codec %s\n", s.substr(pos, e - pos).c_str()); return 1; }
                c.CodecPriority[n++] = cc; c.EnabledCodecMask |= PNA2DP_CODEC_BIT(cc);
                pos = e + 1;
            }
        } else if (!_stricmp(k, "enable") || !_stricmp(k, "disable")) {
            std::string s(v); size_t pos = 0;
            while (pos <= s.size()) {
                size_t e = s.find(',', pos); if (e == std::string::npos) e = s.size();
                ULONG cc = CodecFromName(s.substr(pos, e - pos).c_str());
                if (!cc) { fprintf(stderr, "unknown codec\n"); return 1; }
                if (!_stricmp(k, "enable")) c.EnabledCodecMask |= PNA2DP_CODEC_BIT(cc); else c.EnabledCodecMask &= ~PNA2DP_CODEC_BIT(cc);
                pos = e + 1;
            }
        } else if (!_stricmp(k, "sbc.bitpool")) c.Sbc.MaxBitpool = (UCHAR)atoi(v);
        else if (!_stricmp(k, "sbc.minbitpool")) c.Sbc.MinBitpool = (UCHAR)atoi(v);
        else if (!_stricmp(k, "sbc.override")) c.Sbc.AllowBitpoolOverride = (UCHAR)atoi(v);
        else if (!_stricmp(k, "sbc.dual")) { if (atoi(v)) c.Flags |= PNA2DP_CFG_DUAL_CHANNEL_PREFER; else c.Flags &= ~PNA2DP_CFG_DUAL_CHANNEL_PREFER; }
        else if (!_stricmp(k, "sbc.rates")) { ULONG m; if (!ParseRates(v, &m)) return 1; c.Sbc.RateMask = (UCHAR)(((m & PNA2DP_RATE_16000) ? 0x80 : 0) | ((m & PNA2DP_RATE_32000) ? 0x40 : 0) | ((m & PNA2DP_RATE_44100) ? 0x20 : 0) | ((m & PNA2DP_RATE_48000) ? 0x10 : 0)); }
        else if (!_stricmp(k, "sbc.subbands")) c.Sbc.SubbandMask = atoi(v) == 4 ? PNA2DP_SBC_SUBBANDS_4 : PNA2DP_SBC_SUBBANDS_8;
        else if (!_stricmp(k, "sbc.blocks")) c.Sbc.BlockMask = atoi(v) == 4 ? PNA2DP_SBC_BLOCKS_4 : atoi(v) == 8 ? PNA2DP_SBC_BLOCKS_8 : atoi(v) == 12 ? PNA2DP_SBC_BLOCKS_12 : PNA2DP_SBC_BLOCKS_16;
        else if (!_stricmp(k, "sbc.alloc")) c.Sbc.AllocMask = _stricmp(v, "snr") == 0 ? PNA2DP_SBC_ALLOC_SNR : PNA2DP_SBC_ALLOC_LOUDNESS;
        else if (!_stricmp(k, "aac.bitrate")) c.Aac.BitrateBps = (ULONG)atol(v);
        else if (!_stricmp(k, "aac.vbr")) c.Aac.Vbr = (UCHAR)atoi(v);
        else if (!_stricmp(k, "ldac.quality")) c.Ldac.Eqmid = !_stricmp(v, "hq") ? 0 : !_stricmp(v, "sq") ? 1 : !_stricmp(v, "mq") ? 2 : PNA2DP_LDAC_EQMID_ABR;
        else if (!_stricmp(k, "ldac.abr")) c.Ldac.AbrEnable = (UCHAR)atoi(v);
        else if (!_stricmp(k, "ldac.bits")) c.Ldac.FormatMask = atoi(v) == 16 ? PNA2DP_FMT_S16 : atoi(v) == 24 ? PNA2DP_FMT_S24 : PNA2DP_FMT_S32;
        else if (!_stricmp(k, "ldac.rates")) { ULONG m; if (!ParseRates(v, &m)) return 1; c.Ldac.RateMask = m; }
        else if (!_stricmp(k, "abr")) flag(PNA2DP_CFG_ABR_ENABLE);
        else if (!_stricmp(k, "latency")) c.TargetLatencyMs = (ULONG)atol(v);
        else if (!_stricmp(k, "maxlatency")) c.MaxLatencyMs = (ULONG)atol(v);
        else if (!_stricmp(k, "allrates")) flag(PNA2DP_CFG_EXPOSE_ALL_RATES);
        else if (!_stricmp(k, "swvolume")) flag(PNA2DP_CFG_SOFTWARE_VOLUME);
        else if (!_stricmp(k, "delayreport")) flag(PNA2DP_CFG_DELAY_REPORTING);
        else if (!_stricmp(k, "suspendonsco")) flag(PNA2DP_CFG_SUSPEND_ON_SCO);
        else if (!_stricmp(k, "hostonly")) flag(PNA2DP_CFG_HOST_CODECS_ONLY);
        else if (!_stricmp(k, "autoquality")) flag(PNA2DP_CFG_AUTO_QUALITY);
        else if (!_stricmp(k, "abrfloor")) c.AbrFloorBps = (ULONG)strtoul(v, NULL, 10);
        else if (!_stricmp(k, "applylive")) flag(PNA2DP_CFG_APPLY_LIVE);
        else { fprintf(stderr, "unknown key %s\n", k); return 1; }
    }
    if (!Ioctl(h, IOCTL_PNA2DP_SET_CONFIG, &c, sizeof(c), NULL, 0)) return 1;
    if (!Ioctl(h, IOCTL_PNA2DP_GET_CONFIG, NULL, 0, &c, sizeof(c))) return 1;
    printf("configuration updated\n");
    PrintConfig(c);
    return 0;
}

static int WindowsCommand(int argc, char **argv)
{
    const char *action = argc >= 3 ? argv[2] : "list";
    int index = 0;
    if (argc > 3) {
        if (argc != 5 || strcmp(argv[3], "-d")) { fprintf(stderr, "Usage: pna2dpctl windows [list|status|spatial] [-d N]\n"); return 1; }
        char *end = nullptr; long value = strtol(argv[4], &end, 10);
        if (!argv[4][0] || *end || value < 0 || value > INT_MAX) return 1;
        index = (int)value;
    }
    if (strcmp(action, "list") && strcmp(action, "status") && strcmp(action, "spatial")) return 1;
    auto nodes = pn::EnumerateA2dpSinkNodes();
    if (!strcmp(action, "list")) {
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto &n = nodes[i];
            auto audio = pn::QueryWindowsAudio(n.instanceId);
            printf("[%zu] %ls (%012llX) | %s | %s\n", i, n.name.c_str(), n.address,
                n.restartRequired ? "Driver change pending restart" : n.usesWindowsA2dp() ? "Windows mode" : n.usesPnA2dp() ? "PN-A2DP driver mode" : "Other driver",
                audio.active ? "Output active" : "No active output");
        }
        if (nodes.empty()) puts("No paired Bluetooth A2DP devices");
        return 0;
    }
    if (index >= (int)nodes.size()) { fprintf(stderr, "Bluetooth device index unavailable\n"); return 1; }
    const auto &node = nodes[index];
    printf("Device: %ls\nDriver: %ls %ls\nInstance: %ls\n", node.name.c_str(), node.driverProvider.c_str(), node.driverVersion.c_str(), node.instanceId.c_str());
    if (!strcmp(action, "spatial")) {
        auto info = pn::QuerySpatialAudio(node.instanceId);
        printf("%ls", pn::SpatialReport(info).c_str()); return info.queried ? 0 : 1;
    }
    auto info = pn::QueryWindowsAudio(node.instanceId);
    printf("%ls", pn::WindowsAudioReport(info).c_str()); return info.active ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: pna2dpctl windows [list|status|spatial] [-d N]\n       pna2dpctl list|status|spatial|quality|caps|config|auto|set|watch|stats|trace|reconnect [-d index] [key=value ...]\n"); return 1; }
    const char *cmd = argv[1];
    if (!strcmp(cmd, "windows")) return WindowsCommand(argc, argv);
    int dev = 0, first = 2;
    if (argc > 3 && !strcmp(argv[2], "-d")) { dev = atoi(argv[3]); first = 4; }

    if (!strcmp(cmd, "list")) {
        auto list = EnumerateDevices();
        if (list.empty()) { printf("no PN-A2DP devices\n"); return 0; }
        for (size_t i = 0; i < list.size(); i++) {
            HANDLE h = CreateFileW(list[i].c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            PNA2DP_STATUS s = {};
            if (h != INVALID_HANDLE_VALUE) { Ioctl(h, IOCTL_PNA2DP_GET_STATUS, NULL, 0, &s, sizeof(s)); CloseHandle(h); }
            printf("[%zu] %ls (%012llX) %s %s\n", i, s.RemoteName, s.RemoteAddress, StateName(s.State), CodecName(s.Codec));
        }
        return 0;
    }
    if (!strcmp(cmd, "spatial")) {
        const auto paths = EnumerateDevices();
        if (dev < 0 || dev >= (int)paths.size()) { fprintf(stderr, "PN-A2DP device index unavailable\n"); return 1; }
        for (const auto &iface : pn::EnumerateControlInterfaces()) {
            if (_wcsicmp(iface.path.c_str(), paths[dev].c_str())) continue;
            auto info = pn::QuerySpatialAudio(iface.instanceId);
            wprintf(L"%s", pn::SpatialReport(info).c_str());
            return info.queried ? 0 : 1;
        }
        fprintf(stderr, "Could not resolve the selected device instance\n"); return 1;
    }
    HANDLE h = OpenDevice(dev);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int rc = 0;
    if (!strcmp(cmd, "status")) PrintStatus(h);
    else if (!strcmp(cmd, "quality")) {
        PNA2DP_QUALITY q = {};
        if (!Ioctl(h, IOCTL_PNA2DP_GET_QUALITY, NULL, 0, &q, sizeof(q)) || q.Size != sizeof(q)) rc = 1;
        else {
            const auto paths = EnumerateDevices();
            if (dev >= 0 && dev < (int)paths.size()) for (const auto &iface : pn::EnumerateControlInterfaces())
                if (!_wcsicmp(iface.path.c_str(), paths[dev].c_str())) wprintf(L"Adapter: %s\r\n", pn::AdapterNameForDevice(iface.instanceId).c_str());
            wprintf(L"%s", pn::QualityReport(q).c_str());
        }
    }
    else if (!strcmp(cmd, "caps")) PrintCaps(h);
    else if (!strcmp(cmd, "config")) { PNA2DP_CONFIG c = {}; if (Ioctl(h, IOCTL_PNA2DP_GET_CONFIG, NULL, 0, &c, sizeof(c))) PrintConfig(c); }
    else if (!strcmp(cmd, "auto")) {
        PNA2DP_CONFIG c = {};
        if (!Ioctl(h, IOCTL_PNA2DP_GET_CONFIG, NULL, 0, &c, sizeof(c))) rc = 1;
        else {
            pn::UseAutomaticCodecs(c);
            if (!Ioctl(h, IOCTL_PNA2DP_SET_CONFIG, &c, sizeof(c), NULL, 0)) rc = 1;
            else { puts("Automatic codec selection enabled; only supported codecs and available encoders are used."); PrintConfig(c); }
        }
    }
    else if (!strcmp(cmd, "set")) rc = DoSet(h, argc, argv, first);
    else if (!strcmp(cmd, "stats")) PrintStats(h);
    else if (!strcmp(cmd, "trace")) {
        PNA2DP_TRACE trace = {};
        DWORD bytes = 0;
        if (!Ioctl(h, IOCTL_PNA2DP_GET_TRACE, NULL, 0, &trace, sizeof(trace), &bytes)) rc = 1;
        else if (bytes != sizeof(trace) || trace.Size != sizeof(trace) || trace.Count > PNA2DP_TRACE_ENTRIES) rc = 1;
        else for (ULONG i = 0; i < trace.Count; ++i) {
            const auto &e = trace.Entries[i];
            printf("%lu %llu ms L%lu %.*s\n", e.Sequence, e.TimeHns / 10000, e.Level, (int)PNA2DP_TRACE_TEXT_BYTES, e.Text);
        }
    }
    else if (!strcmp(cmd, "reconnect")) { if (Ioctl(h, IOCTL_PNA2DP_RECONNECT, NULL, 0, NULL, 0)) printf("reconnecting\n"); }
    else if (!strcmp(cmd, "watch")) {
        PrintStatus(h);
        for (;;) {
            ULONG ev = 0;
            if (!Ioctl(h, IOCTL_PNA2DP_WAIT_EVENT, NULL, 0, &ev, sizeof(ev))) break;
            printf("\n-- event 0x%02x%s%s%s%s%s%s --\n", ev, (ev & PNA2DP_EVENT_STATE_CHANGED) ? " state" : "", (ev & PNA2DP_EVENT_CAPS_CHANGED) ? " caps" : "",
                   (ev & PNA2DP_EVENT_CONFIG_CHANGED) ? " config" : "", (ev & PNA2DP_EVENT_SCO_CHANGED) ? " sco" : "", (ev & PNA2DP_EVENT_DELAY_REPORT) ? " delay" : "",
                   (ev & PNA2DP_EVENT_ERROR) ? " error" : "");
            PrintStatus(h);
        }
    } else { fprintf(stderr, "unknown command %s\n", cmd); rc = 1; }
    CloseHandle(h);
    return rc;
}
