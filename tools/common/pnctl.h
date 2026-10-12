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
std::wstring   NtStatusText(ULONG status);
std::wstring   DescribeSep(const PNA2DP_REMOTE_SEP &sep);   /* multi-line decode of the codec IEs */
std::wstring   HexBytes(const UCHAR *p, ULONG n);
std::wstring   AdapterNameForDevice(const std::wstring &instanceId);
const wchar_t *QualityHealthName(ULONG health);
std::wstring   QualityReport(const PNA2DP_QUALITY &q);

/* ---- configuration helpers -------------------------------------------------- */
void  ConfigDefaults(PNA2DP_CONFIG &c);        /* identical to the driver's RegCfgDefaults */
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

/* Presets applied on top of a configuration (codec priority + per codec knobs). */
enum class Preset { BestQuality = 0, Balanced, LowLatency, Compatibility, SbcXq };
const wchar_t *PresetName(Preset p);
const wchar_t *PresetDescription(Preset p);
void ApplyPreset(PNA2DP_CONFIG &c, Preset p);

/* Round trip of a configuration through text ("key=value" lines, pna2dpctl syntax where it
 * exists, plus a hex dump of the whole structure for exact restore). */
std::wstring ConfigToText(const PNA2DP_CONFIG &c);
bool         ConfigFromText(const std::wstring &text, PNA2DP_CONFIG &c);
// UTF-8, maximum 1 MiB. Failed reads preserve c; failed writes preserve the old file.
DWORD        ReadConfigFile(const std::wstring &path, PNA2DP_CONFIG &c);
DWORD        WriteConfigFile(const std::wstring &path, const PNA2DP_CONFIG &c);

} // namespace pn
