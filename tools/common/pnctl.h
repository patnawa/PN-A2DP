/*
 * pnctl.h - user-mode wrapper around the PN-A2DP control interface.
 *
 * Shared by the control panel, the installer and (optionally) the command line tool.
 * Everything here is plain Win32; no exceptions cross the API.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <windows.h>
#include <string>
#include <vector>
#include "pna2dp_public.h"

namespace pn {

/* One control interface published by the driver (one per headset it serves). */
struct ControlInterface {
    std::wstring path;        /* \\?\BTHENUM#...#{guid}\pna2dpctl */
    std::wstring instanceId;  /* BTHENUM\{0000110b-...}\... */
};

std::vector<ControlInterface> EnumerateControlInterfaces();

/* Open handle to one device's control interface. All IOCTLs are issued overlapped so that
 * a pended WAIT_EVENT can be cancelled from another thread. */
class Device {
public:
    Device() = default;
    ~Device();
    Device(const Device &) = delete;
    Device &operator=(const Device &) = delete;

    bool Open(const std::wstring &path, bool writeAccess = true);
    void Close();
    bool IsOpen() const { return m_h != INVALID_HANDLE_VALUE; }
    DWORD LastError() const { return m_err; }

    bool GetVersion(PNA2DP_VERSION &v);
    bool GetStatus(PNA2DP_STATUS &s);
    bool GetCapabilities(PNA2DP_CAPABILITIES &c);
    bool GetConfig(PNA2DP_CONFIG &c);
    bool SetConfig(const PNA2DP_CONFIG &c);
    bool GetStats(PNA2DP_STATS &s);
    bool GetQuality(PNA2DP_QUALITY &q);
    bool ResetStats();
    bool Reconnect();

    /* Blocks until the driver signals an event or timeoutMs elapses (error WAIT_TIMEOUT) or
     * Cancel() is called (error ERROR_OPERATION_ABORTED). Meant for a worker thread: pass errOut
     * to get the error without touching LastError(), which belongs to the calling thread's peers. */
    bool WaitEvent(ULONG &mask, DWORD timeoutMs, DWORD *errOut = nullptr);
    void Cancel();

private:
    bool Ioctl(DWORD code, const void *in, DWORD inLen, void *out, DWORD outLen, DWORD timeoutMs = INFINITE, DWORD *errOut = nullptr);
    HANDLE m_h = INVALID_HANDLE_VALUE;
    DWORD m_err = 0;
};

/* ---- names and formatting --------------------------------------------------- */
const wchar_t *CodecName(ULONG codec);         /* "LDAC" */
const wchar_t *CodecKey(ULONG codec);          /* "ldac" (pna2dpctl key) */
ULONG          CodecFromKey(const wchar_t *k);
const wchar_t *StateName(ULONG state);
std::wstring   RateMaskText(ULONG mask);       /* "44.1k, 48k" */
std::wstring   ChannelModeMaskText(ULONG mask);
std::wstring   FormatMaskText(ULONG mask);
std::wstring   BtAddressText(ULONGLONG addr);  /* "E8:EC:4A:12:34:56" */
std::wstring   StatusFlagsText(ULONG flags);
/* "48000 Hz (Windows 44100 Hz, converted), 2 ch, 24-bit in": the stream format and, when the driver
 * converts (0.1.0.22 EngineSampleRate; older drivers report 0) and withEngineRate is set, the
 * Windows rate it converts from */
std::wstring   StreamFormatText(const PNA2DP_STATUS &status, bool withEngineRate = true);
/* The Windows rate on its own: "44100 Hz, converted to 48000 Hz", "48000 Hz, no conversion",
 * or "-" without a Windows stream (and from drivers before 0.1.0.22) */
std::wstring   EngineRateText(const PNA2DP_STATUS &status);
std::wstring   NtStatusText(ULONG status);
std::wstring   DescribeSep(const PNA2DP_REMOTE_SEP &sep);   /* multi-line decode of the codec IEs */
std::wstring   SepRatesText(const PNA2DP_REMOTE_SEP &sep);  /* the receiver's sample rates for this codec: "44.1k, 48k, 96k", or "-" */
std::wstring   HexBytes(const UCHAR *p, ULONG n);
std::wstring   AdapterNameForDevice(const std::wstring &instanceId);
const wchar_t *QualityHealthName(ULONG health);
std::wstring   QualityReport(const PNA2DP_QUALITY &q);

/* ---- speaker delay (driver 0.1.0.25) ----------------------------------------- */
/* Where the speaker delay the driver applies comes from (PNA2DP_STATUS.SinkDelayUs with the
 * SINK_DELAY_SET / SINK_DELAY_IGNORED flags; transport_policy.h PnSinkDelayDecide). Legacy: a
 * driver before 0.1.0.25 (SinkDelayUs 0 with a stream or a report), described as before. */
enum class SinkDelaySource { NoStream, Set, Reported, Default, Legacy };
SinkDelaySource SinkDelaySourceOf(const PNA2DP_STATUS &s);
bool           DriverAppliesSinkDelay(const PNA2DP_VERSION &v);   /* 0.1.0.25 or later: SinkDelayUs, SinkDelayMs */
std::wstring   DelayMsText(ULONG us);                    /* "364 ms", "25.4 ms" (tenths as reported) */
/* One sentence for the Headset page and pna2dpctl status: "Using 364 ms reported by the
 * speaker", "Using 280 ms, the fixed speaker delay; the speaker reports 364 ms", "Using 200 ms,
 * the LDAC default; the speaker's report of 25 ms is ignored as impossible", "No stream" */
std::wstring   SinkDelayText(const PNA2DP_STATUS &s);
/* The same for the Status page's value column: "364 ms reported by the speaker", "280 ms fixed;
 * the speaker reports 364 ms", "200 ms default; the 25 ms report is ignored", "no stream" */
std::wstring   SinkDelayShortText(const PNA2DP_STATUS &s);
/* The setting (PNA2DP_CONFIG.SinkDelayMs / SinkDelayCodec): "automatic", "280 ms (LDAC)",
 * "280 ms (any codec)" */
std::wstring   SinkDelaySettingText(const PNA2DP_CONFIG &c);
/* The pna2dpctl keys (also in the settings text): sinkdelay=auto|0|1..1000 (auto resets the
 * codec too) and sinkdelay.codec=any|sbc|aac|aptx|aptxhd|aptxll|ldac|... (case-insensitive).
 * Returns 1 when applied, 0 for a refused value (c unchanged), -1 for another key. */
int            SinkDelayKey(PNA2DP_CONFIG &c, const wchar_t *key, const wchar_t *value);
/* The speaker delay the driver is expected to apply while a measurement plays (its lag range,
 * pnlatency.h FitLatencyLagRange), in ms rounded up: the larger of the applied SinkDelayUs and
 * a fixed SinkDelayMs (c: the driver's configuration) for the stream's codec or any codec; with
 * no stream (the measurement may start a fresh one, its codec not known yet) any fixed delay
 * counts. 0 when nothing is known yet (automatic, no stream). */
ULONG          ExpectedSinkDelayMs(const PNA2DP_STATUS &s, const PNA2DP_CONFIG &c);

/* ---- configuration helpers -------------------------------------------------- */
void  ConfigDefaults(PNA2DP_CONFIG &c);        /* the driver's RegCfgDefaults (config_policy.h PnConfigDefaults) */
void  UseAutomaticCodecs(PNA2DP_CONFIG &c);    /* reset codec restrictions, keep volume/transport */
bool  SinkSupportsCodec(const PNA2DP_CAPABILITIES &caps, ULONG codec);
bool  CanActivateCodec(const PNA2DP_CAPABILITIES &caps, const PNA2DP_VERSION &version, ULONG codec);
bool  PreferCodec(PNA2DP_CONFIG &config, const PNA2DP_CAPABILITIES &caps, const PNA2DP_VERSION &version, ULONG codec);
std::wstring CodecSettingsText(const PNA2DP_CONFIG &config, ULONG codec);
ULONG SbcReceiverMaxBitpool(const PNA2DP_CAPABILITIES &caps, ULONG seid = 0);
std::wstring CodecPageStatus(const PNA2DP_CAPABILITIES &caps, const PNA2DP_STATUS &status, ULONG codec);
ULONG SbcFrameBytes(ULONG subbands, ULONG blocks, ULONG channelMode /*PNA2DP_SBC_CH_*/, ULONG bitpool);
ULONG SbcBitrateBps(ULONG sampleRate, ULONG subbands, ULONG blocks, ULONG channelMode, ULONG bitpool);
ULONG LdacBitrateBps(ULONG eqmid, ULONG sampleRate);

/* Latency profiles (config_policy.h PN_LATENCY_PROFILE_*): a TargetLatencyMs / MaxLatencyMs
 * pair. Music 150 / 450 ms rides out radio hiccups without lowering quality, Balanced 60 / 250
 * (default), Low latency 30 / 120 (video, games); any other pair is Custom. */
ULONG          LatencyProfileOf(const PNA2DP_CONFIG &c);
void           ApplyLatencyProfile(PNA2DP_CONFIG &c, ULONG profile);   /* Custom: no change */
const wchar_t *LatencyProfileKey(ULONG profile);    /* "low", "balanced", "music", "custom" */
const wchar_t *LatencyProfileName(ULONG profile);   /* "Low latency", "Balanced", "Music", "Custom" */
bool           LatencyProfileFromKey(const wchar_t *key, ULONG &profile);   /* also "lowlatency"; case-insensitive */

/* Presets applied on top of a configuration (codec priority + per codec knobs). */
enum class Preset { BestQuality = 0, Balanced, LowLatency, Compatibility, SbcXq };
const wchar_t *PresetName(Preset p);
const wchar_t *PresetDescription(Preset p);
void ApplyPreset(PNA2DP_CONFIG &c, Preset p);
bool PresetFromKey(const wchar_t *key, Preset &p);  /* best, defaults, lowlatency, compat, sbcxq; not the latency profile words */

/* Round trip of a configuration through text ("key=value" lines, pna2dpctl syntax where it
 * exists, plus a hex dump of the whole structure for exact restore). */
std::wstring ConfigToText(const PNA2DP_CONFIG &c);
bool         ConfigFromText(const std::wstring &text, PNA2DP_CONFIG &c);
// UTF-8, maximum 1 MiB. Failed reads preserve c; failed writes preserve the old file.
DWORD        ReadConfigFile(const std::wstring &path, PNA2DP_CONFIG &c);
DWORD        WriteConfigFile(const std::wstring &path, const PNA2DP_CONFIG &c);
DWORD        WriteUtf8FileAtomic(const std::wstring &path, const std::wstring &text);

} // namespace pn
