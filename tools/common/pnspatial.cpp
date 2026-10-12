/* SPDX-License-Identifier: MIT */
#define NOMINMAX
#include "pnspatial.h"
#include "pnaudio.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <appmodel.h>
#include <roapi.h>
#include <winrt/Windows.Media.Audio.h>
#include <wrl/client.h>
#include <vector>
#include <sstream>
#include <iomanip>
#include <memory>

namespace pn {
using Microsoft::WRL::ComPtr;
using namespace winrt::Windows::Media::Audio;
namespace {
struct Apartment {
    HRESULT hr = RoInitialize(RO_INIT_SINGLETHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) RoUninitialize(); }
};
SpatialSupport PackageInstalled(const wchar_t *family)
{
    UINT32 count = 0, length = 0;
    LONG error = GetPackagesByPackageFamily(family, &count, nullptr, &length, nullptr);
    if (error == ERROR_SUCCESS || error == ERROR_INSUFFICIENT_BUFFER)
        return count ? SpatialSupport::Supported : SpatialSupport::Unsupported;
    return SpatialSupport::Unknown;
}
winrt::hstring FormatId(SpatialFormat format)
{
    switch (format) {
    case SpatialFormat::Sonic: return SpatialAudioFormatSubtype::WindowsSonic();
    case SpatialFormat::AtmosHeadphones: return SpatialAudioFormatSubtype::DolbyAtmosForHeadphones();
    case SpatialFormat::DtsHeadphones: return SpatialAudioFormatSubtype::DTSHeadphoneX();
    case SpatialFormat::AtmosTheater: return SpatialAudioFormatSubtype::DolbyAtmosForHomeTheater();
    case SpatialFormat::DtsTheater: return SpatialAudioFormatSubtype::DTSXForHomeTheater();
    default: return {};
    }
}
bool EqualFormat(const std::wstring &a, const std::wstring &b)
{
    return !a.empty() && !b.empty() && !_wcsicmp(a.c_str(), b.c_str());
}
bool IsOff(const std::wstring &id)
{
    return id.empty() || EqualFormat(id, L"{00000000-0000-0000-0000-000000000000}");
}
}

SpatialInfo QuerySpatialAudio(const std::wstring &a2dpInstanceId)
{
    SpatialInfo info;
    info.dolbyApp = PackageInstalled(L"DolbyLaboratories.DolbyAccess_rz1tebttyb220");
    info.dtsApp = PackageInstalled(L"DTSInc.DTSSoundUnbound_t5j2fzbtdg37r");
    if (a2dpInstanceId.empty()) { info.error = E_INVALIDARG; info.detail = L"Select a Bluetooth device."; return info; }
    Apartment apartment;
    if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) {
        info.error = apartment.hr; info.detail = L"Windows spatial audio initialization failed."; return info;
    }
    try {
        const auto output = QueryWindowsAudio(a2dpInstanceId);
        info.endpointId = output.endpointId;
        info.endpointName = output.endpointName;
        info.endpointState = output.active ? DEVICE_STATE_ACTIVE : 0;
        info.formFactor = output.formFactor;
        info.interfaceId = output.interfaceId;
        if (!output.active || info.interfaceId.empty()) {
            info.error = FAILED(output.error) ? output.error : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
            info.detail = output.active ? L"Windows output interface unavailable. Refresh after reconnecting." : output.detail;
            return info;
        }
        auto config = SpatialAudioDeviceConfiguration::GetForDeviceId(info.interfaceId);
        info.spatialSupported = config.IsSpatialAudioSupported();
        info.activeFormat = config.ActiveSpatialAudioFormat();
        info.selectedFormat = config.DefaultSpatialAudioFormat();
        info.queried = true;
        for (size_t i = 0; i < info.formats.size(); ++i) {
            auto &f = info.formats[i];
            try {
                auto id = FormatId(static_cast<SpatialFormat>(i));
                f.id = id;
                f.support = config.IsSpatialAudioFormatSupported(id) ? SpatialSupport::Supported : SpatialSupport::Unsupported;
            } catch (winrt::hresult_error const &e) { f.error = e.code(); }
        }
        info.detail = L"Windows reports endpoint compatibility. Dolby/DTS activation or a license may still be required.";
    } catch (winrt::hresult_error const &e) {
        info.error = e.code();
        info.detail = L"Windows spatial query failed: " + std::wstring(e.message());
    }
    return info;
}

const wchar_t *SpatialFormatName(SpatialFormat format)
{
    switch (format) {
    case SpatialFormat::Sonic: return L"Windows Sonic for Headphones";
    case SpatialFormat::AtmosHeadphones: return L"Dolby Atmos for Headphones";
    case SpatialFormat::DtsHeadphones: return L"DTS Headphone:X";
    case SpatialFormat::AtmosTheater: return L"Dolby Atmos for home theater";
    case SpatialFormat::DtsTheater: return L"DTS:X for home theater";
    default: return L"Unknown";
    }
}
std::wstring SpatialActiveName(const SpatialInfo &info, bool selected)
{
    if (!info.queried) return L"Unavailable";
    const auto &id = selected ? info.selectedFormat : info.activeFormat;
    if (IsOff(id)) return L"Off";
    for (size_t i = 0; i < info.formats.size(); ++i)
        if (EqualFormat(id, info.formats[i].id)) return SpatialFormatName(static_cast<SpatialFormat>(i));
    return L"Other Windows provider: " + id;
}
std::wstring SpatialSupportText(const SpatialInfo &info, SpatialFormat format)
{
    // These are transport limits even if a third-party provider reports otherwise.
    if (format == SpatialFormat::AtmosTheater || format == SpatialFormat::DtsTheater) return L"Unavailable over A2DP";
    size_t index = static_cast<size_t>(format);
    if (index >= info.formats.size() || !info.queried) return L"Not detected";
    const auto &f = info.formats[index];
    if (f.support == SpatialSupport::Unknown) return L"Query unavailable";
    if (EqualFormat(info.activeFormat, f.id)) return L"Active";
    if (f.support == SpatialSupport::Unsupported) return L"Not supported by Windows";
    if (EqualFormat(info.selectedFormat, f.id)) return L"Selected; not active";
    return L"Supported; not active";
}
std::wstring SpatialAppText(SpatialSupport installed)
{
    return installed == SpatialSupport::Supported ? L"Installed (license not checked)" :
        installed == SpatialSupport::Unsupported ? L"Not installed for this user" : L"Installation unknown";
}
std::wstring SpatialReport(const SpatialInfo &info)
{
    std::wostringstream text;
    text << L"Endpoint: " << info.endpointName << L"\nID: " << info.endpointId << L"\nInterface: " << info.interfaceId
         << L"\nForm factor: " << info.formFactor << L"\nSpatial query: " << (info.queried ? L"OK" : L"unavailable")
         << L"\nActive: " << SpatialActiveName(info) << L"\nSelected: " << SpatialActiveName(info, true) << L"\n";
    for (size_t i = 0; i < info.formats.size(); ++i) {
        const auto &f = info.formats[i];
        text << SpatialFormatName(static_cast<SpatialFormat>(i)) << L": " << SpatialSupportText(info, static_cast<SpatialFormat>(i))
             << L" (Windows: " << (f.support == SpatialSupport::Supported ? L"yes" : f.support == SpatialSupport::Unsupported ? L"no" : L"unknown")
             << L", HRESULT 0x" << std::hex << static_cast<unsigned>(f.error) << std::dec << L")\n";
    }
    text << L"Dolby Access: " << SpatialAppText(info.dolbyApp) << L"\nDTS Sound Unbound: " << SpatialAppText(info.dtsApp)
         << L"\n" << info.detail << L" (HRESULT 0x" << std::hex << static_cast<unsigned>(info.error) << L")\n"
         << L"Headphone spatial rendering -> stereo PCM -> negotiated A2DP codec; no extra Bluetooth channels.\n"
         << L"Home theater Atmos/DTS:X bitstreams cannot be transported by PN-A2DP; use a compatible HDMI/eARC path.\n";
    return text.str();
}
}
