/* Windows spatial sound discovery; no endpoint, provider or license changes.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <windows.h>
#include <array>
#include <string>

namespace pn {
enum class SpatialFormat { Sonic, AtmosHeadphones, DtsHeadphones, AtmosTheater, DtsTheater, Count };
enum class SpatialSupport { Unknown, Unsupported, Supported };
struct SpatialFormatInfo {
    std::wstring id;
    SpatialSupport support = SpatialSupport::Unknown;
    HRESULT error = S_OK;
};
struct SpatialInfo {
    std::wstring endpointId, interfaceId, endpointName;
    DWORD endpointState = 0, formFactor = ~0u;
    HRESULT error = S_OK;
    std::wstring detail;
    bool queried = false, spatialSupported = false;
    std::wstring activeFormat, selectedFormat;
    std::array<SpatialFormatInfo, static_cast<size_t>(SpatialFormat::Count)> formats;
    SpatialSupport dolbyApp = SpatialSupport::Unknown, dtsApp = SpatialSupport::Unknown;
};
/* Exact PnP ancestor match, never the default endpoint or a friendly-name match.
 * Calls are read-only and may be made from STA or MTA; no persistent COM objects. */
SpatialInfo QuerySpatialAudio(const std::wstring &a2dpInstanceId);
const wchar_t *SpatialFormatName(SpatialFormat format);
std::wstring SpatialActiveName(const SpatialInfo &info, bool selected = false);
std::wstring SpatialSupportText(const SpatialInfo &info, SpatialFormat format);
std::wstring SpatialAppText(SpatialSupport installed);
std::wstring SpatialReport(const SpatialInfo &info);
}
