/* Windows audio controls shared by both driver modes. SPDX-License-Identifier: MIT */
#pragma once
#include <windows.h>
#include <string>

namespace pn {
struct WindowsAudioInfo {
    std::wstring endpointId, endpointName, interfaceId, detail;
    HRESULT error = E_PENDING, volumeError = E_PENDING, muteError = E_PENDING, formatError = E_PENDING;
    DWORD formFactor = ~0u;
    bool active = false, defaultKnown = false, isDefault = false, muted = false, floatingPoint = false;
    float volume = 0;
    UINT32 sampleRate = 0;
    WORD channels = 0, bits = 0;
};

// Exact PnP ancestry match, active render endpoints only. Never uses a friendly
// name or silently falls back to the default output. Each call owns its COM scope.
WindowsAudioInfo QueryWindowsAudio(const std::wstring &a2dpInstanceId);
// expectedEndpointId prevents an old slider interaction targeting a recreated output.
HRESULT SetWindowsVolume(const std::wstring &a2dpInstanceId, const std::wstring &expectedEndpointId, float scalar);
HRESULT SetWindowsMute(const std::wstring &a2dpInstanceId, const std::wstring &expectedEndpointId, bool muted);
std::wstring WindowsAudioFormat(const WindowsAudioInfo &info);
std::wstring WindowsAudioReport(const WindowsAudioInfo &info);
}
