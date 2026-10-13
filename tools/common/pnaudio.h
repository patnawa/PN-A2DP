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
// Own the identifiers before entering COM, which may dispatch UI messages that
// replace the caller's selected device or endpoint strings.
WindowsAudioInfo QueryWindowsAudio(std::wstring a2dpInstanceId);
// expectedEndpointId prevents an old slider interaction targeting a recreated output.
HRESULT SetWindowsVolume(std::wstring a2dpInstanceId, std::wstring expectedEndpointId, float scalar);
HRESULT SetWindowsMute(std::wstring a2dpInstanceId, std::wstring expectedEndpointId, bool muted);
std::wstring WindowsAudioFormat(const WindowsAudioInfo &info);
std::wstring WindowsAudioReport(const WindowsAudioInfo &info);

// KSPROPSETID_BtAudio one-shot request as Windows' own Connect / Disconnect send it: a
// GET of KSPROPERTY_ONESHOT_RECONNECT or _DISCONNECT on the topology filter, no value.
struct BluetoothOneShotRequest { GUID set; ULONG id; ULONG flags; };
BluetoothOneShotRequest BluetoothAudioOneShotRequest(bool connect);
// Where a BluetoothAudioOneShot request stopped.
enum class OneShotStage {
    Lookup,     // not sent: no instance, Windows audio could not be enumerated or an endpoint
                // could not be checked, or several endpoints match
    NoEndpoint, // not sent: every render endpoint was checked and none belongs to this device
    Topology,   // not sent: the device's endpoint was found, its topology filter was not reached
    Driver,     // the property reached the driver; the HRESULT is the driver's answer
};
// Asks the Bluetooth audio driver of this A2DP device instance (PN-A2DP or the Windows
// inbox driver) to connect or disconnect the receiver, through the render endpoint's
// topology filter (IDeviceTopology -> IConnector -> IPart -> IKsControl). The endpoint may
// be active, unplugged or disabled; a not-present endpoint is never used.
// The caller must have initialized COM on this thread (any apartment). Returns quickly: the
// driver only records the request. S_OK means the request was accepted, not that the
// receiver (dis)connected; the endpoint state and IsConnected report the outcome.
// Errors: E_INVALIDARG (no instance), HRESULT_FROM_WIN32(ERROR_NOT_FOUND) with stage
// NoEndpoint (no endpoint of this device), ERROR_DUP_NAME (ambiguous endpoints), the
// enumeration or topology error, or the driver's error (stage Driver).
HRESULT BluetoothAudioOneShot(const std::wstring &a2dpInstanceId, bool connect, OneShotStage *stage = nullptr);
// One sentence describing a BluetoothAudioOneShot result for the panel and the CLI.
// pnDriver: the device uses PN-A2DP, so an old-driver rejection names the driver version.
std::wstring BluetoothAudioOneShotText(HRESULT hr, OneShotStage stage, bool connect, bool pnDriver);
// PN-A2DP's IOCTL_PNA2DP_RECONNECT resets the link (disconnect first). It replaces a Connect
// only when Windows has no output for the device at all and the driver reports its session
// down (PNA2DP_STATE_IDLE or _ERROR), never while it is connecting or connected.
bool OneShotReconnectFallback(bool connect, OneShotStage stage, bool statusKnown, ULONG sessionState);

}
