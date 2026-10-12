/*
 * pnctl.cpp - user-mode wrapper around the PN-A2DP control interface.
 * SPDX-License-Identifier: MIT
 */
#include "pnctl.h"
#include <cfgmgr32.h>
#include <devpkey.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>

#pragma comment(lib, "cfgmgr32.lib")

namespace pn {

/* ------------------------------------------------------------------------- */
/* Enumeration                                                                  */
/* ------------------------------------------------------------------------- */

std::vector<ControlInterface> EnumerateControlInterfaces()
{
    std::vector<ControlInterface> out;
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || len < 2)
        return out;
    std::vector<WCHAR> buf(len);
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS)
        return out;
    for (PWCHAR p = buf.data(); *p; p += wcslen(p) + 1) {
        ControlInterface ci;
        ci.path = p;
        DEVPROPTYPE type = 0;
        WCHAR id[MAX_DEVICE_ID_LEN] = {};
        ULONG size = sizeof(id);
        if (CM_Get_Device_Interface_PropertyW(p, &DEVPKEY_Device_InstanceId, &type, (PBYTE)id, &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING)
            ci.instanceId = id;
        out.push_back(ci);
    }
    return out;
}

/* ------------------------------------------------------------------------- */
/* Device                                                                       */
/* ------------------------------------------------------------------------- */

Device::~Device() { Close(); }

bool Device::Open(const std::wstring &path, bool writeAccess)
{
    Close();
    DWORD access = GENERIC_READ | (writeAccess ? GENERIC_WRITE : 0);
    m_h = CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (m_h == INVALID_HANDLE_VALUE) { m_err = GetLastError(); return false; }
    m_err = 0;
    return true;
}

void Device::Close()
{
    if (m_h != INVALID_HANDLE_VALUE) {
        CancelIoEx(m_h, NULL);
        CloseHandle(m_h);
        m_h = INVALID_HANDLE_VALUE;
    }
}

bool Device::Ioctl(DWORD code, const void *in, DWORD inLen, void *out, DWORD outLen, DWORD timeoutMs, DWORD *errOut)
{
    DWORD err = 0;
    bool result = false;
    HANDLE h = m_h;
    if (h == INVALID_HANDLE_VALUE) { err = ERROR_INVALID_HANDLE; goto done; }
    {
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!ov.hEvent) { err = GetLastError(); goto done; }
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(h, code, (LPVOID)in, inLen, out, outLen, &ret, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            DWORD w = WaitForSingleObject(ov.hEvent, timeoutMs);
            if (w == WAIT_TIMEOUT) CancelIoEx(h, &ov);
            /* the request may have completed between the timeout and the cancel: honour its result */
            ok = GetOverlappedResult(h, &ov, &ret, TRUE);
            if (!ok) { err = GetLastError(); if (w == WAIT_TIMEOUT && err == ERROR_OPERATION_ABORTED) err = WAIT_TIMEOUT; }
        } else if (!ok) {
            err = GetLastError();
        }
        CloseHandle(ov.hEvent);
        result = ok != FALSE;
    }
done:
    /* m_err serves the UI thread; a caller on another thread passes its own errOut */
    if (errOut) *errOut = err; else m_err = err;
    return result;
}

bool Device::GetVersion(PNA2DP_VERSION &v)      { ZeroMemory(&v, sizeof(v)); return Ioctl((DWORD)IOCTL_PNA2DP_GET_VERSION, NULL, 0, &v, sizeof(v)); }
bool Device::GetStatus(PNA2DP_STATUS &s)        { ZeroMemory(&s, sizeof(s)); return Ioctl((DWORD)IOCTL_PNA2DP_GET_STATUS, NULL, 0, &s, sizeof(s)); }
bool Device::GetCapabilities(PNA2DP_CAPABILITIES &c) { ZeroMemory(&c, sizeof(c)); return Ioctl((DWORD)IOCTL_PNA2DP_GET_CAPABILITIES, NULL, 0, &c, sizeof(c)); }
bool Device::GetConfig(PNA2DP_CONFIG &c)        { ZeroMemory(&c, sizeof(c)); return Ioctl((DWORD)IOCTL_PNA2DP_GET_CONFIG, NULL, 0, &c, sizeof(c)); }
bool Device::SetConfig(const PNA2DP_CONFIG &c)  { return Ioctl((DWORD)IOCTL_PNA2DP_SET_CONFIG, &c, sizeof(c), NULL, 0); }
bool Device::GetStats(PNA2DP_STATS &s)          { ZeroMemory(&s, sizeof(s)); return Ioctl((DWORD)IOCTL_PNA2DP_GET_STATS, NULL, 0, &s, sizeof(s)); }
bool Device::GetQuality(PNA2DP_QUALITY &q)
{
    ZeroMemory(&q, sizeof(q));
    if (!Ioctl((DWORD)IOCTL_PNA2DP_GET_QUALITY, NULL, 0, &q, sizeof(q))) return false;
    if (q.Size != sizeof(q)) { m_err = ERROR_INVALID_DATA; return false; }
    return true;
}
bool Device::ResetStats()                       { return Ioctl((DWORD)IOCTL_PNA2DP_RESET_STATS, NULL, 0, NULL, 0); }
bool Device::Reconnect()                        { return Ioctl((DWORD)IOCTL_PNA2DP_RECONNECT, NULL, 0, NULL, 0); }

bool Device::WaitEvent(ULONG &mask, DWORD timeoutMs, DWORD *errOut)
{
    mask = 0;
    return Ioctl((DWORD)IOCTL_PNA2DP_WAIT_EVENT, NULL, 0, &mask, sizeof(mask), timeoutMs, errOut);
}

void Device::Cancel()
{
    if (m_h != INVALID_HANDLE_VALUE) CancelIoEx(m_h, NULL);
}

/* ------------------------------------------------------------------------- */
/* Names                                                                        */
/* ------------------------------------------------------------------------- */

const wchar_t *CodecName(ULONG c)
{
    switch (c) {
    case PNA2DP_CODEC_SBC: return L"SBC";
    case PNA2DP_CODEC_AAC: return L"AAC";
    case PNA2DP_CODEC_APTX: return L"aptX";
    case PNA2DP_CODEC_APTX_HD: return L"aptX HD";
    case PNA2DP_CODEC_APTX_LL: return L"aptX Low Latency";
    case PNA2DP_CODEC_LDAC: return L"LDAC";
    case PNA2DP_CODEC_FASTSTREAM: return L"FastStream";
    case PNA2DP_CODEC_OPUS: return L"Opus";
    case PNA2DP_CODEC_LC3PLUS: return L"LC3plus";
    default: return L"none";
    }
}

const wchar_t *CodecKey(ULONG c)
{
    switch (c) {
    case PNA2DP_CODEC_SBC: return L"sbc";
    case PNA2DP_CODEC_AAC: return L"aac";
    case PNA2DP_CODEC_APTX: return L"aptx";
    case PNA2DP_CODEC_APTX_HD: return L"aptxhd";
    case PNA2DP_CODEC_APTX_LL: return L"aptxll";
    case PNA2DP_CODEC_LDAC: return L"ldac";
    case PNA2DP_CODEC_FASTSTREAM: return L"faststream";
    case PNA2DP_CODEC_OPUS: return L"opus";
    case PNA2DP_CODEC_LC3PLUS: return L"lc3plus";
    default: return L"none";
    }
}

ULONG CodecFromKey(const wchar_t *k)
{
    for (ULONG c = 1; c < PNA2DP_CODEC_MAX; c++) if (_wcsicmp(CodecKey(c), k) == 0) return c;
    if (!_wcsicmp(k, L"aptx-hd")) return PNA2DP_CODEC_APTX_HD;
    if (!_wcsicmp(k, L"aptx-ll")) return PNA2DP_CODEC_APTX_LL;
    return 0;
}

const wchar_t *StateName(ULONG s)
{
    static const wchar_t *n[] = { L"Idle", L"Connecting", L"Discovering", L"Configured", L"Open", L"Streaming", L"Suspended", L"Closing", L"Error" };
    return s < 9 ? n[s] : L"?";
}

std::wstring RateMaskText(ULONG m)
{
    static const struct { ULONG b; const wchar_t *s; } t[] = {
        { PNA2DP_RATE_8000, L"8k" }, { PNA2DP_RATE_11025, L"11.025k" }, { PNA2DP_RATE_12000, L"12k" }, { PNA2DP_RATE_16000, L"16k" },
        { PNA2DP_RATE_22050, L"22.05k" }, { PNA2DP_RATE_24000, L"24k" }, { PNA2DP_RATE_32000, L"32k" }, { PNA2DP_RATE_44100, L"44.1k" },
        { PNA2DP_RATE_48000, L"48k" }, { PNA2DP_RATE_64000, L"64k" }, { PNA2DP_RATE_88200, L"88.2k" }, { PNA2DP_RATE_96000, L"96k" } };
    std::wstring r;
    for (auto &e : t) if (m & e.b) { if (!r.empty()) r += L", "; r += e.s; }
    return r.empty() ? L"any" : r;
}

std::wstring ChannelModeMaskText(ULONG m)
{
    std::wstring r;
    auto add = [&](const wchar_t *s) { if (!r.empty()) r += L", "; r += s; };
    if (m & PNA2DP_CHMODE_MONO) add(L"mono");
    if (m & PNA2DP_CHMODE_DUAL) add(L"dual");
    if (m & PNA2DP_CHMODE_STEREO) add(L"stereo");
    if (m & PNA2DP_CHMODE_JOINT) add(L"joint");
    return r.empty() ? L"any" : r;
}

std::wstring FormatMaskText(ULONG m)
{
    std::wstring r;
    auto add = [&](const wchar_t *s) { if (!r.empty()) r += L", "; r += s; };
    if (m & PNA2DP_FMT_S16) add(L"16-bit");
    if (m & PNA2DP_FMT_S24) add(L"24-bit");
    if (m & PNA2DP_FMT_S32) add(L"32-bit");
    if (m & PNA2DP_FMT_F32) add(L"float");
    return r.empty() ? L"auto" : r;
}

std::wstring BtAddressText(ULONGLONG a)
{
    wchar_t b[32];
    swprintf_s(b, L"%02llX:%02llX:%02llX:%02llX:%02llX:%02llX", (a >> 40) & 0xFF, (a >> 32) & 0xFF, (a >> 24) & 0xFF, (a >> 16) & 0xFF, (a >> 8) & 0xFF, a & 0xFF);
    return b;
}

std::wstring StatusFlagsText(ULONG f)
{
    std::wstring r;
    auto add = [&](const wchar_t *s) { if (!r.empty()) r += L", "; r += s; };
    if (f & PNA2DP_STATUS_FLAG_AUDIO_ACTIVE) add(L"audio running");
    if (f & PNA2DP_STATUS_FLAG_SCO_ACTIVE) add(L"call (SCO) active");
    if (f & PNA2DP_STATUS_FLAG_ABR_ACTIVE) add(L"bitrate reduced by ABR");
    if (f & PNA2DP_STATUS_FLAG_DELAY_REPORTING) add(L"delay reporting");
    if (f & PNA2DP_STATUS_FLAG_REMOTE_INITIATED) add(L"remote initiated");
    if (f & PNA2DP_STATUS_FLAG_HOST_MISSING) add(L"codec host missing");
    if (f & PNA2DP_STATUS_FLAG_FALLBACK_SBC) add(L"SBC fallback");
    if (f & PNA2DP_STATUS_FLAG_RADIO_IN_RANGE) add(L"in range");
    return r.empty() ? L"-" : r;
}

std::wstring NtStatusText(ULONG status)
{
    wchar_t b[320];
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    wchar_t msg[256] = {};
    DWORD n = nt ? FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, nt, status, 0, msg, 255, NULL) : 0;
    while (n && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L' ')) msg[--n] = 0;
    swprintf_s(b, n ? L"0x%08X (%s)" : L"0x%08X", status, msg);
    return b;
}

std::wstring HexBytes(const UCHAR *p, ULONG n)
{
    std::wstring r;
    wchar_t b[8];
    for (ULONG i = 0; i < n; i++) { swprintf_s(b, L"%02X ", p[i]); r += b; }
    if (!r.empty()) r.pop_back();
    return r;
}

std::wstring DescribeSep(const PNA2DP_REMOTE_SEP &s)
{
    std::wstring r;
    wchar_t b[256];
    const UCHAR *ie = s.CodecInfo;
    ULONG n = s.CodecInfoLength;
    swprintf_s(b, L"SEID %u, %s, %s%s\r\n", s.Seid, s.TsepIsSink ? L"sink" : L"source", CodecName(s.Codec), s.InUse ? L" (in use)" : L"");
    r += b;
    if (s.Codec == PNA2DP_CODEC_NONE || s.VendorId) {
        swprintf_s(b, L"Vendor ID 0x%08X, codec ID 0x%04X\r\n", s.VendorId, s.VendorCodecId);
        r += b;
    }
    auto list = [&](const wchar_t *title, std::wstring v) { r += title; r += L": "; r += v; r += L"\r\n"; };
    auto masktext = [](const UCHAR v, const UCHAR *bits, const wchar_t *const *names, int count) {
        std::wstring t;
        for (int i = 0; i < count; i++) if (v & bits[i]) { if (!t.empty()) t += L", "; t += names[i]; }
        return t.empty() ? std::wstring(L"-") : t;
    };
    switch (s.Codec) {
    case PNA2DP_CODEC_SBC: {
        if (n < 4) break;
        static const UCHAR rb[] = { 0x80, 0x40, 0x20, 0x10 }; static const wchar_t *rn[] = { L"16k", L"32k", L"44.1k", L"48k" };
        static const UCHAR cb[] = { 0x08, 0x04, 0x02, 0x01 }; static const wchar_t *cn[] = { L"mono", L"dual", L"stereo", L"joint" };
        static const UCHAR bb[] = { 0x80, 0x40, 0x20, 0x10 }; static const wchar_t *bn[] = { L"4", L"8", L"12", L"16" };
        static const UCHAR sb[] = { 0x08, 0x04 }; static const wchar_t *sn[] = { L"4", L"8" };
        static const UCHAR ab[] = { 0x02, 0x01 }; static const wchar_t *an[] = { L"SNR", L"loudness" };
        list(L"Sample rates", masktext(ie[0], rb, rn, 4));
        list(L"Channel modes", masktext(ie[0], cb, cn, 4));
        list(L"Blocks", masktext(ie[1], bb, bn, 4));
        list(L"Subbands", masktext(ie[1], sb, sn, 2));
        list(L"Allocation", masktext(ie[1], ab, an, 2));
        swprintf_s(b, L"%u .. %u (max %u kbps joint stereo at 48 kHz)", ie[2], ie[3], SbcBitrateBps(48000, 8, 16, PNA2DP_SBC_CH_JOINT, ie[3]) / 1000);
        list(L"Bitpool", b);
        break;
    }
    case PNA2DP_CODEC_AAC: {
        if (n < 6) break;
        static const UCHAR ob[] = { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02 };
        static const wchar_t *on[] = { L"MPEG-2 AAC LC", L"MPEG-4 AAC LC", L"MPEG-4 AAC LTP", L"MPEG-4 AAC Scalable", L"HE-AAC v1", L"HE-AAC v2", L"AAC ELD v2" };
        list(L"Object types", masktext(ie[0], ob, on, 7));
        std::wstring rates;
        static const struct { int byte; UCHAR bit; const wchar_t *s; } rt[] = { {1,0x80,L"8k"},{1,0x40,L"11.025k"},{1,0x20,L"12k"},{1,0x10,L"16k"},{1,0x08,L"22.05k"},{1,0x04,L"24k"},{1,0x02,L"32k"},{1,0x01,L"44.1k"},
                                                                               {2,0x80,L"48k"},{2,0x40,L"64k"},{2,0x20,L"88.2k"},{2,0x10,L"96k"} };
        for (auto &e : rt) if (ie[e.byte] & e.bit) { if (!rates.empty()) rates += L", "; rates += e.s; }
        list(L"Sample rates", rates.empty() ? L"-" : rates);
        static const UCHAR cb[] = { 0x08, 0x04 }; static const wchar_t *cn[] = { L"mono", L"stereo" };
        list(L"Channels", masktext(ie[2], cb, cn, 2));
        ULONG br = ((ie[3] & 0x7F) << 16) | (ie[4] << 8) | ie[5];
        swprintf_s(b, L"%s, max %u kbps", (ie[3] & 0x80) ? L"VBR supported" : L"CBR only", br / 1000);
        list(L"Bitrate", b);
        break;
    }
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_HD:
    case PNA2DP_CODEC_APTX_LL: {
        if (n < 7) break;
        static const UCHAR rb[] = { 0x80, 0x40, 0x20, 0x10 }; static const wchar_t *rn[] = { L"16k", L"32k", L"44.1k", L"48k" };
        static const UCHAR cb[] = { 0x02, 0x01 }; static const wchar_t *cn[] = { L"stereo", L"mono" };
        list(L"Sample rates", masktext(ie[6], rb, rn, 4));
        list(L"Channels", masktext(ie[6], cb, cn, 2));
        if (s.Codec == PNA2DP_CODEC_APTX_LL && n >= 8) list(L"Bidirectional link", (ie[7] & 0x01) ? L"yes" : L"no");
        if (s.Codec == PNA2DP_CODEC_APTX_HD) list(L"Input", L"24-bit");
        break;
    }
    case PNA2DP_CODEC_LDAC: {
        if (n < 8) break;
        static const UCHAR rb[] = { 0x20, 0x10, 0x08, 0x04 }; static const wchar_t *rn[] = { L"44.1k", L"48k", L"88.2k", L"96k" };
        static const UCHAR cb[] = { 0x04, 0x02, 0x01 }; static const wchar_t *cn[] = { L"mono", L"dual", L"stereo" };
        list(L"Sample rates", masktext(ie[6], rb, rn, 4));
        list(L"Channel modes", masktext(ie[7], cb, cn, 3));
        list(L"Bitrates", L"990/909, 660/606, 330/303 kbps (HQ, SQ, MQ) + adaptive");
        break;
    }
    default:
        break;
    }
    std::wstring svc;
    auto adds = [&](const wchar_t *t) { if (!svc.empty()) svc += L", "; svc += t; };
    if (s.ServiceCaps & PNA2DP_SVC_MEDIA_TRANSPORT) adds(L"media transport");
    if (s.ServiceCaps & PNA2DP_SVC_REPORTING) adds(L"reporting");
    if (s.ServiceCaps & PNA2DP_SVC_RECOVERY) adds(L"recovery");
    if (s.ServiceCaps & PNA2DP_SVC_CONTENT_PROTECTION) adds(L"content protection (SCMS-T)");
    if (s.ServiceCaps & PNA2DP_SVC_HEADER_COMPRESSION) adds(L"header compression");
    if (s.ServiceCaps & PNA2DP_SVC_MULTIPLEXING) adds(L"multiplexing");
    if (s.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) adds(L"delay reporting");
    list(L"Services", svc.empty() ? L"-" : svc);
    list(L"Raw IEs", HexBytes(ie, n));
    return r;
}

/* ------------------------------------------------------------------------- */
/* Configuration helpers                                                        */
/* ------------------------------------------------------------------------- */

void ConfigDefaults(PNA2DP_CONFIG &c)
{
    ZeroMemory(&c, sizeof(c));
    c.Size = sizeof(c);
    c.Version = PNA2DP_ABI_VERSION;
    c.CodecPriority[0] = PNA2DP_CODEC_LDAC;
    c.CodecPriority[1] = PNA2DP_CODEC_APTX_HD;
    c.CodecPriority[2] = PNA2DP_CODEC_AAC;
    c.CodecPriority[3] = PNA2DP_CODEC_APTX;
    c.CodecPriority[4] = PNA2DP_CODEC_APTX_LL;
    c.CodecPriority[5] = PNA2DP_CODEC_SBC;
    c.EnabledCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) |
                         PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC);
    c.Flags = PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_APPLY_LIVE | PNA2DP_CFG_SUSPEND_ON_SCO | PNA2DP_CFG_DELAY_REPORTING | PNA2DP_CFG_SOFTWARE_VOLUME | PNA2DP_CFG_AUTO_QUALITY;
    c.TargetLatencyMs = 60;
    c.MaxLatencyMs = 250;
    c.AbrFloorBps = 128000;
    c.VolumeDb16 = 0;
    c.ReconnectDelayMs = 2000;
    c.Sbc.MinBitpool = 2;
    c.Sbc.MaxBitpool = 0;
    c.Aac.Vbr = 1;
    c.Aac.BitrateBps = 0;
    c.Ldac.Eqmid = PNA2DP_LDAC_EQMID_SQ;
    c.Ldac.AbrEnable = 1;
}

ULONG SbcFrameBytes(ULONG nsb, ULONG nblk, ULONG chmode, ULONG bp)
{
    ULONG nch = (chmode == PNA2DP_SBC_CH_MONO) ? 1 : 2;
    ULONG len = 4 + (4 * nsb * nch) / 8;
    if (chmode == PNA2DP_SBC_CH_MONO || chmode == PNA2DP_SBC_CH_DUAL) len += (nblk * nch * bp + 7) / 8;
    else if (chmode == PNA2DP_SBC_CH_STEREO) len += (nblk * bp + 7) / 8;
    else len += (nsb + nblk * bp + 7) / 8;
    return len;
}

ULONG SbcBitrateBps(ULONG fs, ULONG nsb, ULONG nblk, ULONG chmode, ULONG bp)
{
    if (!nsb || !nblk) return 0;
    return (ULONG)(8ull * SbcFrameBytes(nsb, nblk, chmode, bp) * fs / (nsb * nblk));
}

void UseAutomaticCodecs(PNA2DP_CONFIG &c)
{
    PNA2DP_CONFIG defaults;
    ConfigDefaults(defaults);
    memcpy(c.CodecPriority, defaults.CodecPriority, sizeof(c.CodecPriority));
    c.EnabledCodecMask = defaults.EnabledCodecMask;
    c.Sbc = defaults.Sbc; c.Aac = defaults.Aac; c.Aptx = defaults.Aptx;
    c.AptxHd = defaults.AptxHd; c.AptxLl = defaults.AptxLl; c.Ldac = defaults.Ldac;
    c.Ldac.Eqmid = PNA2DP_LDAC_EQMID_HQ; // begin at highest quality; ABR handles congestion
    c.Aac.BitrateBps = 320000; c.Aac.Vbr = 0; // bounded by the receiver's advertised maximum
    c.Flags &= ~(PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_HOST_CODECS_ONLY | PNA2DP_CFG_EXPOSE_ALL_RATES);
    c.Flags |= PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_APPLY_LIVE | PNA2DP_CFG_SOFTWARE_VOLUME;
}

ULONG SbcReceiverMaxBitpool(const PNA2DP_CAPABILITIES &caps, ULONG seid)
{
    ULONG maximum = 0;
    for (ULONG i = 0; i < caps.SepCount && i < _countof(caps.Seps); ++i) {
        const auto &s = caps.Seps[i];
        if (s.Codec != PNA2DP_CODEC_SBC || !s.TsepIsSink || s.MediaType || s.CodecInfoLength < 4) continue;
        const UCHAR *ie = s.CodecInfo;
        if (!(ie[0] & 0xf0) || !(ie[0] & 0x0f) || !(ie[1] & 0xf0) || !(ie[1] & 0x0c) || !(ie[1] & 3) ||
            ie[2] < 2 || ie[3] < ie[2] || ie[3] > 250) continue;
        if (seid && s.Seid == seid) return ie[3];
        maximum = (std::max)(maximum, (ULONG)ie[3]);
    }
    return maximum; // zero means unknown, never an assumed 53
}

std::wstring CodecPageStatus(const PNA2DP_CAPABILITIES &caps, const PNA2DP_STATUS &status, ULONG codec)
{
    std::wstring result = std::wstring(CodecName(codec)) + L": ";
    result += !caps.SepCount ? L"receiver support unknown" : SinkSupportsCodec(caps, codec) ? L"receiver supported" : L"not supported by this receiver";
    const bool negotiated = status.Codec && status.State >= PNA2DP_STATE_CONFIGURED && status.State <= PNA2DP_STATE_SUSPENDED;
    if (negotiated && status.Codec == codec) {
        result += L". " + std::wstring(status.State == PNA2DP_STATE_STREAMING ? L"Live: " : L"Negotiated: ") +
            std::to_wstring(status.BitrateBps / 1000) + L" kbps, " + std::to_wstring(status.SampleRate) + L" Hz, " +
            std::to_wstring(status.Channels) + L" channels (" + StateName(status.State) + L").";
    } else {
        result += L". Not active.";
        if (negotiated) result += L" Current output: " + std::wstring(CodecName(status.Codec)) + L".";
    }
    return result;
}

ULONG LdacBitrateBps(ULONG eqmid, ULONG fs)
{
    bool hi = (fs == 48000 || fs == 96000);
    switch (eqmid) {
    case PNA2DP_LDAC_EQMID_HQ: return hi ? 990000 : 909000;
    case PNA2DP_LDAC_EQMID_SQ: return hi ? 660000 : 606000;
    case PNA2DP_LDAC_EQMID_MQ: return hi ? 330000 : 303000;
    default: return hi ? 660000 : 606000;
    }
}

bool SinkSupportsCodec(const PNA2DP_CAPABILITIES &caps, ULONG codec)
{
    if (!codec || codec >= PNA2DP_CODEC_MAX) return false;
    for (ULONG i = 0; i < caps.SepCount && i < PNA2DP_MAX_REMOTE_SEPS; ++i)
        if (caps.Seps[i].TsepIsSink && !caps.Seps[i].MediaType && caps.Seps[i].Codec == codec) return true;
    return false;
}

bool CanActivateCodec(const PNA2DP_CAPABILITIES &caps, const PNA2DP_VERSION &v, ULONG codec)
{
    return SinkSupportsCodec(caps, codec) && ((v.KernelCodecMask | v.HostCodecMask) & PNA2DP_CODEC_BIT(codec));
}

bool PreferCodec(PNA2DP_CONFIG &c, const PNA2DP_CAPABILITIES &caps, const PNA2DP_VERSION &v, ULONG codec)
{
    if (!CanActivateCodec(caps, v, codec)) return false;
    ULONG order[8] = {codec};
    ULONG count = 1;
    for (ULONG old : c.CodecPriority) {
        if (!old || old >= PNA2DP_CODEC_MAX) continue;
        bool duplicate = false;
        for (ULONG i = 0; i < count; ++i) if (order[i] == old) duplicate = true;
        if (!duplicate && count < 8) order[count++] = old;
    }
    bool sbc = false;
    for (ULONG k : order) if (k == PNA2DP_CODEC_SBC) sbc = true;
    if (!sbc) order[count < 8 ? count : 7] = PNA2DP_CODEC_SBC;
    memcpy(c.CodecPriority, order, sizeof(order));
    c.EnabledCodecMask |= PNA2DP_CODEC_BIT(codec) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    c.Flags |= PNA2DP_CFG_APPLY_LIVE;
    return true;
}

std::wstring CodecSettingsText(const PNA2DP_CONFIG &c, ULONG codec)
{
    auto number = [](ULONG n) { return std::to_wstring(n); };
    switch (codec) {
    case PNA2DP_CODEC_SBC:
        return L"Bitpool " + number(c.Sbc.MinBitpool ? c.Sbc.MinBitpool : 2) + L".." +
            (c.Sbc.MaxBitpool ? number(c.Sbc.MaxBitpool) : std::wstring(L"receiver max")) +
            ((c.Flags & PNA2DP_CFG_DUAL_CHANNEL_PREFER) ? L"; dual preferred" : L"; auto mode");
    case PNA2DP_CODEC_AAC:
        return (c.Aac.BitrateBps ? number(c.Aac.BitrateBps / 1000) + L" kbps" : std::wstring(L"Receiver limit")) +
            (c.Aac.Vbr ? L"; VBR allowed" : L"; CBR");
    case PNA2DP_CODEC_LDAC: {
        const wchar_t *quality = c.Ldac.Eqmid == PNA2DP_LDAC_EQMID_HQ ? L"HQ" : c.Ldac.Eqmid == PNA2DP_LDAC_EQMID_MQ ? L"MQ" :
            c.Ldac.Eqmid == PNA2DP_LDAC_EQMID_SQ ? L"SQ" : L"Adaptive (SQ start)";
        return std::wstring(quality) + L" ceiling; ABR " + (((c.Flags & PNA2DP_CFG_ABR_ENABLE) && c.Ldac.AbrEnable) ? L"on" : L"off");
    }
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_HD:
    case PNA2DP_CODEC_APTX_LL: {
        ULONG rates = codec == PNA2DP_CODEC_APTX_HD ? c.AptxHd.RateMask : codec == PNA2DP_CODEC_APTX_LL ? c.AptxLl.RateMask : c.Aptx.RateMask;
        return (rates ? RateMaskText(rates) : std::wstring(L"Auto rate")) + L"; fixed bitrate";
    }
    default: return L"";
    }
}

const wchar_t *PresetName(Preset p)
{
    switch (p) {
    case Preset::BestQuality: return L"Best quality";
    case Preset::Balanced: return L"Balanced (driver defaults)";
    case Preset::LowLatency: return L"Low latency";
    case Preset::Compatibility: return L"Compatibility (SBC only)";
    case Preset::SbcXq: return L"SBC XQ (high bitpool, dual channel)";
    }
    return L"";
}

const wchar_t *PresetDescription(Preset p)
{
    switch (p) {
    case Preset::BestQuality: return L"LDAC 990 kbps 24-bit first, then aptX HD, AAC 320 kbps, aptX, SBC. ABR on, 80 ms buffer.";
    case Preset::Balanced: return L"LDAC 660 kbps with ABR, then aptX HD, AAC, aptX, aptX LL, SBC. 60 ms buffer.";
    case Preset::LowLatency: return L"aptX Low Latency, aptX, SBC with a 30 ms buffer and ABR; no LDAC/AAC (they buffer more).";
    case Preset::Compatibility: return L"SBC only at the sink's own limits. Use when a headset misbehaves with other codecs.";
    case Preset::SbcXq: return L"SBC only, bitpool 47 with dual channel preferred (SBC XQ+, about 550 kbps at 44.1 kHz). Needs a sink that accepts it.";
    }
    return L"";
}

/* Presets touch only the codec order, the enable mask, the knobs they are about, the buffer
 * sizes and the ABR / dual-channel flags. Everything else (per-codec masks, behaviour flags,
 * volume, reconnect delay) stays as it is; "Balanced" is the full driver default. */
void ApplyPreset(PNA2DP_CONFIG &c, Preset p)
{
    PNA2DP_CONFIG d;
    ConfigDefaults(d);
    auto order = [&](std::initializer_list<ULONG> codecs) {
        ZeroMemory(c.CodecPriority, sizeof(c.CodecPriority));
        c.EnabledCodecMask = 0;
        int i = 0;
        for (ULONG k : codecs) { if (i < 8) c.CodecPriority[i++] = k; c.EnabledCodecMask |= PNA2DP_CODEC_BIT(k); }
    };
    switch (p) {
    case Preset::BestQuality:
        order({ PNA2DP_CODEC_LDAC, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_SBC });
        c.Ldac.Eqmid = PNA2DP_LDAC_EQMID_HQ; c.Ldac.AbrEnable = 1; c.Ldac.FormatMask = PNA2DP_FMT_S24;
        c.Aac.BitrateBps = 320000; c.Aac.Vbr = 0;
        c.Sbc.MaxBitpool = 53; c.Sbc.AllowBitpoolOverride = 0;
        c.TargetLatencyMs = 80; c.MaxLatencyMs = 300;
        c.Flags |= PNA2DP_CFG_ABR_ENABLE; c.Flags &= ~PNA2DP_CFG_DUAL_CHANNEL_PREFER;
        break;
    case Preset::Balanced: {
        LONG volume = c.VolumeDb16;
        c = d;
        c.VolumeDb16 = volume;
        break;
    }
    case Preset::LowLatency:
        order({ PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_APTX, PNA2DP_CODEC_SBC });
        c.TargetLatencyMs = 30; c.MaxLatencyMs = 120;
        c.Sbc.MaxBitpool = 53; c.Sbc.AllowBitpoolOverride = 0;
        c.Flags |= PNA2DP_CFG_ABR_ENABLE; c.Flags &= ~PNA2DP_CFG_DUAL_CHANNEL_PREFER;
        break;
    case Preset::Compatibility:
        order({ PNA2DP_CODEC_SBC });
        c.Sbc = d.Sbc;
        c.Flags &= ~(PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_DUAL_CHANNEL_PREFER);
        c.TargetLatencyMs = d.TargetLatencyMs; c.MaxLatencyMs = d.MaxLatencyMs;
        break;
    case Preset::SbcXq:
        /* dual channel bitpool 47 = 551 kbps at 44.1 kHz (SBC XQ+); joint stereo would need bitpool 94 for the same rate */
        order({ PNA2DP_CODEC_SBC });
        c.Sbc.MaxBitpool = 47; c.Sbc.MinBitpool = 2; c.Sbc.AllowBitpoolOverride = 1;
        c.Sbc.BlockMask = PNA2DP_SBC_BLOCKS_16; c.Sbc.SubbandMask = PNA2DP_SBC_SUBBANDS_8;
        c.Flags |= PNA2DP_CFG_DUAL_CHANNEL_PREFER | PNA2DP_CFG_ABR_ENABLE;
        c.TargetLatencyMs = d.TargetLatencyMs; c.MaxLatencyMs = d.MaxLatencyMs;
        break;
    }
    c.EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
}

/* ---- text round trip ---------------------------------------------------- */

std::wstring AdapterNameForDevice(const std::wstring &instanceId)
{
    // Identify a radio interface on the selected sink's actual ancestor chain.
    // Never label it with the first enumerated adapter on a multi-radio PC.
    GUID radioGuid = { 0x0850302a, 0xb344, 0x4fda, {0x9b,0xe9,0x90,0x57,0x6b,0x8d,0x46,0xf0} };
    DEVINST node;
    if (instanceId.empty() || CM_Locate_DevNodeW(&node, const_cast<PWSTR>(instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return L"Adapter name unavailable";
    for (unsigned depth = 0; depth < 24; ++depth) {
        WCHAR id[MAX_DEVICE_ID_LEN] = {};
        ULONG len = 0;
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            CM_Get_Device_Interface_List_SizeW(&len, &radioGuid, id, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) == CR_SUCCESS && len > 1) {
            WCHAR name[512] = {}; DEVPROPTYPE type; ULONG size = sizeof(name);
            CONFIGRET cr = CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_FriendlyName, &type, (PBYTE)name, &size, 0);
            if (cr != CR_SUCCESS) { size = sizeof(name); cr = CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_DeviceDesc, &type, (PBYTE)name, &size, 0); }
            if (cr == CR_SUCCESS && type == DEVPROP_TYPE_STRING) return name;
            return id;
        }
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    return L"Adapter name unavailable";
}

const wchar_t *QualityHealthName(ULONG health)
{
    switch (health) {
    case PNA2DP_QUALITY_IDLE: return L"Idle - play audio to measure the link";
    case PNA2DP_QUALITY_SAMPLING: return L"Measuring active traffic";
    case PNA2DP_QUALITY_STABLE: return L"Stable in the observed traffic";
    case PNA2DP_QUALITY_CONGESTED: return L"Congestion observed";
    case PNA2DP_QUALITY_RECOVERING: return L"Reduced bitrate - checking recovery";
    default: return L"Unknown";
    }
}

std::wstring QualityReport(const PNA2DP_QUALITY &q)
{
    std::wstring out = L"Local address: " + BtAddressText(q.LocalAddress) + L"\r\n";
    auto num = [](ULONG n) { return std::to_wstring(n); };
    auto feature = [&](unsigned bit) { return (q.LmpFeatures & (1ull << bit)) ? L"yes" : L"no"; };
    if ((q.Flags & PNA2DP_QUALITY_RADIO_VALID) && q.LmpFeatures) {
        out += L"Classic EDR: 2 Mbps " + std::wstring(feature(25)) + L", 3 Mbps " + feature(26) + L"; 5-slot EDR " + feature(40) + L"\r\n";
        out += L"HCI version code " + num(q.HciVersion) + L", LMP version code " + num(q.LmpVersion) + L"; manufacturer ID " + num(q.Manufacturer) + L"\r\n";
        out += L"HCI revision " + num(q.HciRevision) + L", LMP subversion " + num(q.LmpSubversion) + L" (vendor-specific)\r\n";
    } else out += L"Radio capabilities unknown; live transport monitoring remains available. Query: " + NtStatusText(q.RadioQueryStatus) + L"\r\n";
    out += (q.Flags & PNA2DP_QUALITY_CONSERVATIVE) ? L"Policy: conservative start for Basic Rate-only radio (350 kbps budget).\r\n" :
        (q.Flags & PNA2DP_QUALITY_AUTO) ? L"Policy: automatic; receiver support and configured quality ceilings apply.\r\n" : L"Policy: manual quality ceilings.\r\n";
    out += L"Health: " + std::wstring(QualityHealthName(q.Health)) + L"\r\n";
    out += L"Codec: " + std::wstring(CodecName(q.Codec)) + L"; current " + num(q.BitrateBps / 1000) + L" kbps\r\n";
    out += L"Effective range: " + num(q.FloorBps / 1000) + L" - " + num(q.CeilingBps / 1000) + L" kbps; reduction level " + num(q.Level) + L" / " + num(q.MaxLevel) + L"\r\n";
    out += L"Adaptive bitrate: " + std::wstring((q.Flags & PNA2DP_QUALITY_ABR) ? L"enabled" : L"disabled");
    if (q.Codec && !q.MaxLevel) out += L"; no lower bitrate within this codec's limits";
    out += L"\r\nClean traffic: " + num(q.HealthyMs / 1000) + L" s; sustained congestion: " + num(q.CongestedMs) + L" ms\r\n";
    out += L"Last " + num(q.ObservationMs) + L" ms: " + num(q.CompletedPackets) + L" completions, " + num(q.SlowPackets) + L" slow, " + num(q.FailedPackets) + L" failures\r\n";
    out += L"Peak audio backlog in that window: " + num(q.BacklogPeakUs / 1000) + L" ms\r\n\r\n";
    ULONG target = (q.Flags & PNA2DP_QUALITY_CHANGE_DETAILS) && q.RecoveryTargetMs >= 5000 && q.RecoveryTargetMs <= 60000 ? q.RecoveryTargetMs : 30000;
    out += L"A sustained 1 s of congestion can lower bitrate (2 s between steps). Current recovery target: " + num(target / 1000) + L" s of clean traffic.\r\n";
    if (q.Flags & PNA2DP_QUALITY_CHANGE_DETAILS)
        out += L"Continuous recovery steps require 30, then 10, then 5 s; a quick relapse requires 60 s before retrying.\r\n";
    out += L"EDR values describe adapter capabilities, not usable bandwidth or current link speed. Distance, interference, the receiver and host load still matter.\r\n";
    return out;
}

static void AppendLine(std::wstring &out, const wchar_t *fmt, ...)
{
    wchar_t b[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(b, _TRUNCATE, fmt, ap);
    va_end(ap);
    out += b;
    out += L"\r\n";
}

std::wstring ConfigToText(const PNA2DP_CONFIG &c)
{
    std::wstring out;
    AppendLine(out, L"# PN-A2DP configuration. The blob line restores every field exactly; the key=value lines");
    AppendLine(out, L"# (pna2dpctl syntax) are applied on top of it, so editing a key here changes that setting.");
    std::wstring pri;
    for (int i = 0; i < 8 && c.CodecPriority[i]; i++) { if (!pri.empty()) pri += L","; pri += CodecKey(c.CodecPriority[i]); }
    AppendLine(out, L"codecs=%s", pri.c_str());
    std::wstring en;
    for (ULONG k = 1; k < PNA2DP_CODEC_MAX; k++) if (c.EnabledCodecMask & PNA2DP_CODEC_BIT(k)) { if (!en.empty()) en += L","; en += CodecKey(k); }
    AppendLine(out, L"enable=%s", en.c_str());
    AppendLine(out, L"sbc.bitpool=%u", c.Sbc.MaxBitpool);
    AppendLine(out, L"sbc.minbitpool=%u", c.Sbc.MinBitpool);
    AppendLine(out, L"sbc.override=%u", c.Sbc.AllowBitpoolOverride);
    AppendLine(out, L"sbc.dual=%u", (c.Flags & PNA2DP_CFG_DUAL_CHANNEL_PREFER) ? 1 : 0);
    AppendLine(out, L"aac.bitrate=%u", c.Aac.BitrateBps);
    AppendLine(out, L"aac.vbr=%u", c.Aac.Vbr);
    AppendLine(out, L"ldac.quality=%s", c.Ldac.Eqmid == 0 ? L"hq" : c.Ldac.Eqmid == 1 ? L"sq" : c.Ldac.Eqmid == 2 ? L"mq" : L"abr");
    AppendLine(out, L"ldac.abr=%u", c.Ldac.AbrEnable);
    AppendLine(out, L"abr=%u", (c.Flags & PNA2DP_CFG_ABR_ENABLE) ? 1 : 0);
    AppendLine(out, L"latency=%u", c.TargetLatencyMs);
    AppendLine(out, L"maxlatency=%u", c.MaxLatencyMs);
    AppendLine(out, L"allrates=%u", (c.Flags & PNA2DP_CFG_EXPOSE_ALL_RATES) ? 1 : 0);
    AppendLine(out, L"swvolume=%u", (c.Flags & PNA2DP_CFG_SOFTWARE_VOLUME) ? 1 : 0);
    AppendLine(out, L"delayreport=%u", (c.Flags & PNA2DP_CFG_DELAY_REPORTING) ? 1 : 0);
    AppendLine(out, L"suspendonsco=%u", (c.Flags & PNA2DP_CFG_SUSPEND_ON_SCO) ? 1 : 0);
    AppendLine(out, L"autoquality=%u", (c.Flags & PNA2DP_CFG_AUTO_QUALITY) ? 1 : 0);
    AppendLine(out, L"hostonly=%u", (c.Flags & PNA2DP_CFG_HOST_CODECS_ONLY) ? 1 : 0);
    AppendLine(out, L"applylive=%u", (c.Flags & PNA2DP_CFG_APPLY_LIVE) ? 1 : 0);
    std::wstring blob;
    const UCHAR *p = (const UCHAR *)&c;
    wchar_t h[4];
    for (size_t i = 0; i < sizeof(c); i++) { swprintf_s(h, L"%02x", p[i]); blob += h; }
    AppendLine(out, L"blob=%s", blob.c_str());
    return out;
}

static std::wstring Trim(const std::wstring &s)
{
    size_t b = s.find_first_not_of(L" \t\r\n"), e = s.find_last_not_of(L" \t\r\n");
    return b == std::wstring::npos ? L"" : s.substr(b, e - b + 1);
}

static bool ParseNumber(const std::wstring &v, ULONG &out)
{
    if (v.empty()) return false;
    ULONG n = 0;
    for (wchar_t digit : v) {
        if (digit < L'0' || digit > L'9' || n > (MAXDWORD - (digit - L'0')) / 10) return false;
        n = n * 10 + (digit - L'0');
    }
    out = n;
    return true;
}

static bool ParseCodecList(const std::wstring &v, std::vector<ULONG> &out)
{
    out.clear();
    size_t p = 0;
    bool allKnown = true;
    while (p <= v.size()) {
        size_t c = v.find(L',', p); if (c == std::wstring::npos) c = v.size();
        std::wstring name = Trim(v.substr(p, c - p));
        if (!name.empty()) { ULONG cc = CodecFromKey(name.c_str()); if (cc) out.push_back(cc); else allKnown = false; }
        p = c + 1;
    }
    return allKnown;
}

/* The exported blob (when it matches this build's structure) restores every field exactly;
 * key=value lines are then applied on top so that hand edits win. Without a blob the keys are
 * applied over the driver defaults. Returns false when nothing recognisable was found. */
bool ConfigFromText(const std::wstring &text, PNA2DP_CONFIG &c)
{
    PNA2DP_CONFIG n;
    ConfigDefaults(n);
    bool haveBlob = false;
    // Parse actual lines, not the first substring "blob=" (which may be a comment).
    for (size_t pos = 0; pos < text.size();) {
        size_t e = text.find_first_of(L"\r\n", pos);
        std::wstring line = Trim(text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos));
        pos = e == std::wstring::npos ? text.size() : e + 1;
        size_t eq = line.find(L'=');
        if (line.empty() || line[0] == L'#' || eq == std::wstring::npos || _wcsicmp(Trim(line.substr(0, eq)).c_str(), L"blob")) continue;
        std::wstring hex = Trim(line.substr(eq + 1));
        if (haveBlob || hex.size() != sizeof(PNA2DP_CONFIG) * 2) return false;
        PNA2DP_CONFIG tmp = {};
        UCHAR *p = (UCHAR *)&tmp;
        auto nibble = [](wchar_t ch) -> int {
            if (ch >= L'0' && ch <= L'9') return ch - L'0';
            if (ch >= L'a' && ch <= L'f') return ch - L'a' + 10;
            if (ch >= L'A' && ch <= L'F') return ch - L'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < sizeof(tmp); i++) {
            int high = nibble(hex[i * 2]), low = nibble(hex[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            p[i] = (UCHAR)((high << 4) | low);
        }
        if (tmp.Size != sizeof(tmp) || tmp.Version != PNA2DP_ABI_VERSION) return false;
        n = tmp; haveBlob = true;
    }
    size_t pos = 0;
    int recognised = 0;
    while (pos < text.size()) {
        size_t e = text.find_first_of(L"\r\n", pos);
        std::wstring line = Trim(text.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos));
        pos = (e == std::wstring::npos) ? text.size() : e + 1;
        size_t eq = line.find(L'=');
        if (line.empty() || line[0] == L'#' || eq == std::wstring::npos) continue;
        std::wstring k = Trim(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
        for (auto &ch : k) ch = (wchar_t)towlower(ch);
        std::wstring vl = v;
        for (auto &ch : vl) ch = (wchar_t)towlower(ch);
        if (k == L"blob") continue;
        ULONG num = 0;
        bool isNum = ParseNumber(v, num);
        auto flag = [&](ULONG bit) { if (!isNum) return; if (num) n.Flags |= bit; else n.Flags &= ~bit; };
        auto setU8 = [&](UCHAR &f, ULONG max) { if (isNum && num <= max) f = (UCHAR)num; };
        std::vector<ULONG> codecs;
        recognised++;
        if (k == L"codecs") {
            ParseCodecList(v, codecs);
            if (!codecs.empty()) {
                ZeroMemory(n.CodecPriority, sizeof(n.CodecPriority));
                for (size_t i = 0; i < codecs.size() && i < 8; i++) { n.CodecPriority[i] = codecs[i]; n.EnabledCodecMask |= PNA2DP_CODEC_BIT(codecs[i]); }
            }
        } else if (k == L"enable") {
            ParseCodecList(v, codecs);
            n.EnabledCodecMask = 0;
            for (ULONG cc : codecs) n.EnabledCodecMask |= PNA2DP_CODEC_BIT(cc);
        } else if (k == L"disable") {
            ParseCodecList(v, codecs);
            for (ULONG cc : codecs) n.EnabledCodecMask &= ~PNA2DP_CODEC_BIT(cc);
        }
        else if (k == L"sbc.bitpool") setU8(n.Sbc.MaxBitpool, 250);
        else if (k == L"sbc.minbitpool") setU8(n.Sbc.MinBitpool, 250);
        else if (k == L"sbc.override") setU8(n.Sbc.AllowBitpoolOverride, 1);
        else if (k == L"sbc.dual") flag(PNA2DP_CFG_DUAL_CHANNEL_PREFER);
        else if (k == L"aac.bitrate") { if (isNum) n.Aac.BitrateBps = num; }
        else if (k == L"aac.vbr") setU8(n.Aac.Vbr, 1);
        else if (k == L"ldac.quality") { if (vl == L"hq") n.Ldac.Eqmid = 0; else if (vl == L"sq") n.Ldac.Eqmid = 1; else if (vl == L"mq") n.Ldac.Eqmid = 2; else if (vl == L"abr") n.Ldac.Eqmid = PNA2DP_LDAC_EQMID_ABR; }
        else if (k == L"ldac.abr") setU8(n.Ldac.AbrEnable, 1);
        else if (k == L"abr") flag(PNA2DP_CFG_ABR_ENABLE);
        else if (k == L"latency") { if (isNum) n.TargetLatencyMs = num; }
        else if (k == L"maxlatency") { if (isNum) n.MaxLatencyMs = num; }
        else if (k == L"allrates") flag(PNA2DP_CFG_EXPOSE_ALL_RATES);
        else if (k == L"swvolume") flag(PNA2DP_CFG_SOFTWARE_VOLUME);
        else if (k == L"delayreport") flag(PNA2DP_CFG_DELAY_REPORTING);
        else if (k == L"suspendonsco") flag(PNA2DP_CFG_SUSPEND_ON_SCO);
        else if (k == L"autoquality") flag(PNA2DP_CFG_AUTO_QUALITY);
        else if (k == L"hostonly") flag(PNA2DP_CFG_HOST_CODECS_ONLY);
        else if (k == L"applylive") flag(PNA2DP_CFG_APPLY_LIVE);
        else recognised--;   /* unknown keys are ignored */
    }
    if (!haveBlob && recognised == 0) return false;
    n.EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    c = n;
    return true;
}

namespace {
struct ConfigFileHandle {
    HANDLE value;
    explicit ConfigFileHandle(HANDLE file) : value(file) {}
    ~ConfigFileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
}

DWORD ReadConfigFile(const std::wstring &path, PNA2DP_CONFIG &c)
{
    ConfigFileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    if (GetFileType(file.value) != FILE_TYPE_DISK) return ERROR_INVALID_DATA;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size)) return GetLastError();
    if (size.QuadPart <= 0) return ERROR_INVALID_DATA;
    if (size.QuadPart > 1024 * 1024) return ERROR_FILE_TOO_LARGE;
    std::string bytes((size_t)size.QuadPart, '\0');
    DWORD offset = 0;
    while (offset < bytes.size()) {
        DWORD read = 0;
        if (!ReadFile(file.value, &bytes[offset], (DWORD)bytes.size() - offset, &read, nullptr)) return GetLastError();
        if (!read) return ERROR_HANDLE_EOF;
        offset += read;
    }
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), nullptr, 0);
    if (!length) return GetLastError();
    std::wstring text(length, L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), (int)bytes.size(), &text[0], length)) return GetLastError();
    if (!text.empty() && text[0] == 0xfeff) text.erase(0, 1);
    if (text.find(L'\0') != std::wstring::npos) return ERROR_INVALID_DATA;
    PNA2DP_CONFIG parsed{};
    if (!ConfigFromText(text, parsed)) return ERROR_INVALID_DATA;
    c = parsed;
    return ERROR_SUCCESS;
}

DWORD WriteConfigFile(const std::wstring &path, const PNA2DP_CONFIG &c)
{
    return WriteUtf8FileAtomic(path, ConfigToText(c));
}

DWORD WriteUtf8FileAtomic(const std::wstring &path, const std::wstring &text)
{
    if (text.empty() || text.size() > 1024 * 1024) return ERROR_INVALID_DATA;
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (!length) return GetLastError();
    std::string bytes(length, '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), (int)text.size(), &bytes[0], length, nullptr, nullptr)) return GetLastError();
    wchar_t full[MAX_PATH]{}, temporary[MAX_PATH]{};
    DWORD size = GetFullPathNameW(path.c_str(), _countof(full), full, nullptr);
    if (!size) return GetLastError();
    if (size >= _countof(full)) return ERROR_FILENAME_EXCED_RANGE;
    std::wstring destination(full);
    auto separator = destination.find_last_of(L"\\/");
    if (separator == std::wstring::npos || separator == destination.size() - 1) return ERROR_INVALID_NAME;
    std::wstring folder = destination.substr(0, separator + 1);
    if (!GetTempFileNameW(folder.c_str(), L"pna", 0, temporary)) return GetLastError();
    DWORD error = ERROR_SUCCESS;
    {
        ConfigFileHandle file(CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value == INVALID_HANDLE_VALUE) error = GetLastError();
        DWORD offset = 0;
        while (!error && offset < bytes.size()) {
            DWORD written = 0;
            if (!WriteFile(file.value, bytes.data() + offset, (DWORD)bytes.size() - offset, &written, nullptr)) error = GetLastError();
            else if (!written) error = ERROR_WRITE_FAULT;
            else offset += written;
        }
        if (!error && !FlushFileBuffers(file.value)) error = GetLastError();
    }
    // Same-directory replacement only after every byte has been written and flushed.
    if (!error && !MoveFileExW(temporary, full, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) error = GetLastError();
    if (error) DeleteFileW(temporary);
    return error;
}

} // namespace pn
