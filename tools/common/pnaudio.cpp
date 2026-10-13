/* SPDX-License-Identifier: MIT */
#define NOMINMAX
#include "pnaudio.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devicetopology.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include "pna2dp_public.h"
#include <vector>
#include <memory>
#include <cmath>
#include <sstream>
#include <iomanip>

namespace pn {
using Microsoft::WRL::ComPtr;
namespace {
struct Apartment {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~Apartment() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
std::wstring PropertyString(IPropertyStore *store, REFPROPERTYKEY key)
{
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring result;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) result = v.pwszVal;
    PropVariantClear(&v);
    return result;
}
bool DescendsFrom(const std::wstring &endpointInstance, const std::wstring &ancestor)
{
    if (endpointInstance.empty() || ancestor.empty()) return false;
    DEVINST node;
    if (CM_Locate_DevNodeW(&node, const_cast<PWSTR>(endpointInstance.c_str()), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    for (unsigned depth = 0; depth < 16; ++depth) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(node, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) return false;
        if (!_wcsicmp(id, ancestor.c_str())) return true;
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS || parent == node) return false;
        node = parent;
    }
    return false;
}
// S_OK: the endpoint belongs to the device. S_FALSE: it does not. A failure: it could not
// be checked (a transient COM or endpoint-builder failure is not "no match").
HRESULT EndpointMatches(IMMDeviceEnumerator *enumerator, IMMDevice *endpoint, const std::wstring &ancestor)
{
    // Endpoint property stores do not consistently expose PKEY_Device_InstanceId.
    // Follow the documented topology connector to the actual audio adapter.
    ComPtr<IDeviceTopology> topology;
    HRESULT hr, unchecked = S_FALSE;
    if (FAILED(hr = endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void **>(topology.GetAddressOf())))) return hr;
    UINT count = 0;
    if (FAILED(hr = topology->GetConnectorCount(&count))) return hr;
    for (UINT i = 0; i < count && i < 16; ++i) {
        ComPtr<IConnector> connector;
        if (FAILED(hr = topology->GetConnector(i, &connector))) { unchecked = hr; continue; }
        LPWSTR rawId = nullptr;
        // E_NOTFOUND: an unconnected connector leads nowhere, a definite answer.
        if (FAILED(hr = connector->GetDeviceIdConnectedTo(&rawId))) { if (hr != E_NOTFOUND) unchecked = hr; continue; }
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> freeId(rawId, CoTaskMemFree);
        ComPtr<IMMDevice> adapter;
        ComPtr<IPropertyStore> properties;
        if (FAILED(hr = enumerator->GetDevice(rawId, &adapter)) || FAILED(hr = adapter->OpenPropertyStore(STGM_READ, &properties))) {
            unchecked = hr; continue;
        }
        if (DescendsFrom(PropertyString(properties.Get(), PKEY_Device_InstanceId), ancestor)) return S_OK;
    }
    return unchecked;
}
std::wstring RenderInterface(const std::wstring &ancestor)
{
    // WinRT needs the device-interface path. An MMDevice endpoint ID silently
    // returns all formats unsupported, even for a fully working spatial endpoint.
    for (unsigned retry = 0; retry < 3; ++retry) {
        ULONG count = 0;
        if (CM_Get_Device_Interface_List_SizeW(&count, const_cast<GUID *>(&DEVINTERFACE_AUDIO_RENDER), nullptr,
                CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS || count < 2) return {};
        std::vector<wchar_t> paths(count);
        CONFIGRET cr = CM_Get_Device_Interface_ListW(const_cast<GUID *>(&DEVINTERFACE_AUDIO_RENDER), nullptr,
            paths.data(), count, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (cr == CR_BUFFER_SMALL) continue;
        if (cr != CR_SUCCESS || !paths[0]) return {};
        std::wstring result;
        for (size_t offset = 0; offset < paths.size() && paths[offset];) {
            const wchar_t *path = paths.data() + offset;
            size_t length = wcsnlen(path, paths.size() - offset);
            if (length >= paths.size() - offset) return {};
            wchar_t instance[MAX_DEVICE_ID_LEN] = {};
            ULONG size = sizeof(instance);
            DEVPROPTYPE type = 0;
            if (CM_Get_Device_Interface_PropertyW(path, &DEVPKEY_Device_InstanceId, &type,
                    reinterpret_cast<PBYTE>(instance), &size, 0) == CR_SUCCESS && type == DEVPROP_TYPE_STRING &&
                    DescendsFrom(instance, ancestor)) {
                if (!result.empty()) return {}; // Refuse ambiguous outputs.
                result.assign(path, length);
            }
            offset += length + 1;
        }
        return result;
    }
    return {};
}


// The one render endpoint in States whose topology leads to the device instance.
// *unchecked (optional) receives the error of an endpoint that could not be checked, so a
// caller can tell "no endpoint" from "an endpoint might have matched"; untouched if none.
HRESULT MatchEndpoint(IMMDeviceEnumerator *enumerator, const std::wstring &ancestor, DWORD states, ComPtr<IMMDevice> &output,
                      HRESULT *unchecked = nullptr)
{
    ComPtr<IMMDeviceCollection> devices;
    HRESULT hr;
    if (FAILED(hr = enumerator->EnumAudioEndpoints(eRender, states, &devices))) return hr;
    UINT count = 0;
    if (FAILED(hr = devices->GetCount(&count))) return hr;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> candidate;
        if (FAILED(hr = devices->Item(i, &candidate)) || FAILED(hr = EndpointMatches(enumerator, candidate.Get(), ancestor))) {
            if (unchecked && SUCCEEDED(*unchecked)) *unchecked = hr;
            continue;
        }
        if (hr != S_OK) continue;
        if (output) { output.Reset(); return HRESULT_FROM_WIN32(ERROR_DUP_NAME); }
        output = candidate;
    }
    return output ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
}

HRESULT FindOutput(const std::wstring &ancestor, WindowsAudioInfo &info, ComPtr<IMMDevice> &output)
{
    if (ancestor.empty()) return E_INVALIDARG;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    if (FAILED(hr = MatchEndpoint(enumerator.Get(), ancestor, DEVICE_STATE_ACTIVE, output))) return hr;
    LPWSTR id = nullptr;
    if (FAILED(hr = output->GetId(&id))) return hr;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> freeId(id, CoTaskMemFree);
    info.endpointId = id;
    ComPtr<IPropertyStore> properties;
    if (SUCCEEDED(output->OpenPropertyStore(STGM_READ, &properties))) {
        info.endpointName = PropertyString(properties.Get(), PKEY_Device_FriendlyName);
        PROPVARIANT value; PropVariantInit(&value);
        if (SUCCEEDED(properties->GetValue(PKEY_AudioEndpoint_FormFactor, &value)) && value.vt == VT_UI4) info.formFactor = value.ulVal;
        PropVariantClear(&value);
    }
    ComPtr<IMMDevice> defaultOutput;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &defaultOutput);
    if (SUCCEEDED(hr)) {
        LPWSTR defaultId = nullptr;
        if (SUCCEEDED(defaultOutput->GetId(&defaultId))) {
            info.defaultKnown = true;
            info.isDefault = !_wcsicmp(id, defaultId);
            CoTaskMemFree(defaultId);
        }
    } else if (hr == E_NOTFOUND) info.defaultKnown = true;
    info.active = true;
    return S_OK;
}

HRESULT ChangeVolume(const std::wstring &instance, const std::wstring &expected, const float *scalar, const bool *muted)
{
    if (instance.empty() || expected.empty()) return E_INVALIDARG;
    if (scalar && (!std::isfinite(*scalar) || *scalar < 0 || *scalar > 1)) return E_INVALIDARG;
    Apartment apartment;
    if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) return apartment.hr;
    WindowsAudioInfo info;
    ComPtr<IMMDevice> output;
    HRESULT hr = FindOutput(instance, info, output);
    if (FAILED(hr)) return hr;
    if (_wcsicmp(info.endpointId.c_str(), expected.c_str())) return HRESULT_FROM_WIN32(ERROR_RETRY);
    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(hr = output->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void **>(volume.GetAddressOf())))) return hr;
    return scalar ? volume->SetMasterVolumeLevelScalar(*scalar, nullptr) : volume->SetMute(*muted, nullptr);
}
}

WindowsAudioInfo QueryWindowsAudio(std::wstring instance)
{
    WindowsAudioInfo info;
    Apartment apartment;
    if (FAILED(apartment.hr) && apartment.hr != RPC_E_CHANGED_MODE) {
        info.error = apartment.hr; info.detail = L"Windows audio initialization failed."; return info;
    }
    ComPtr<IMMDevice> output;
    info.error = FindOutput(instance, info, output);
    if (FAILED(info.error)) {
        info.detail = info.error == HRESULT_FROM_WIN32(ERROR_DUP_NAME) ? L"Multiple outputs match this device; controls are unavailable."
                    : instance.empty() ? L"Select a Bluetooth device."
                    : info.error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ? L"No active output. Turn on and connect this Bluetooth device."
                    : L"Windows audio query failed. Refresh to retry.";
        return info;
    }
    info.interfaceId = RenderInterface(instance);
    info.detail = L"Connected - Windows audio output is available.";
    ComPtr<IAudioEndpointVolume> volume;
    info.volumeError = output->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void **>(volume.GetAddressOf()));
    info.muteError = info.volumeError;
    if (SUCCEEDED(info.volumeError)) {
        info.volumeError = volume->GetMasterVolumeLevelScalar(&info.volume);
        BOOL muted = FALSE;
        info.muteError = volume->GetMute(&muted);
        if (SUCCEEDED(info.muteError)) info.muted = muted != FALSE;
    }
    ComPtr<IAudioClient> client;
    info.formatError = output->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
        reinterpret_cast<void **>(client.GetAddressOf()));
    if (SUCCEEDED(info.formatError)) {
        WAVEFORMATEX *format = nullptr;
        info.formatError = client->GetMixFormat(&format);
        if (SUCCEEDED(info.formatError) && format) {
            info.sampleRate = format->nSamplesPerSec; info.channels = format->nChannels; info.bits = format->wBitsPerSample;
            info.floatingPoint = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
                auto extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
                info.floatingPoint = IsEqualGUID(extended->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
                if (extended->Samples.wValidBitsPerSample) info.bits = extended->Samples.wValidBitsPerSample;
            }
        }
        CoTaskMemFree(format);
    }
    return info;
}

HRESULT SetWindowsVolume(std::wstring instance, std::wstring expected, float scalar)
{ return ChangeVolume(instance, expected, &scalar, nullptr); }
HRESULT SetWindowsMute(std::wstring instance, std::wstring expected, bool muted)
{ return ChangeVolume(instance, expected, nullptr, &muted); }

BluetoothOneShotRequest BluetoothAudioOneShotRequest(bool connect)
{
    return { KSPROPSETID_BtAudio, ULONG(connect ? KSPROPERTY_ONESHOT_RECONNECT : KSPROPERTY_ONESHOT_DISCONNECT), KSPROPERTY_TYPE_GET };
}

HRESULT BluetoothAudioOneShot(const std::wstring &instance, bool connect, OneShotStage *stage)
{
    OneShotStage ignored;
    OneShotStage &at = stage ? *stage : ignored;
    at = OneShotStage::Lookup;
    if (instance.empty()) return E_INVALIDARG;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    // A disconnected receiver's endpoint is unplugged (or disabled by the user), not active.
    // "No endpoint" needs every active and unplugged endpoint checked: one that could not be
    // checked might be this device's. Disabled endpoints are matched leniently, as before.
    ComPtr<IMMDevice> endpoint;
    HRESULT unchecked = S_OK;
    for (DWORD states : {DWORD(DEVICE_STATE_ACTIVE), DWORD(DEVICE_STATE_UNPLUGGED), DWORD(DEVICE_STATE_DISABLED)}) {
        hr = MatchEndpoint(enumerator.Get(), instance, states, endpoint, states == DEVICE_STATE_DISABLED ? nullptr : &unchecked);
        if (hr != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) break;
    }
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        if (FAILED(unchecked)) return unchecked;
        at = OneShotStage::NoEndpoint;
        return hr;
    }
    if (FAILED(hr)) return hr;
    at = OneShotStage::Topology;
    ComPtr<IDeviceTopology> topology;
    if (FAILED(hr = endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void **>(topology.GetAddressOf())))) return hr;
    // The endpoint's connector leads to the bridge pin of the driver's topology filter;
    // KSPROPSETID_BtAudio is a property of that filter.
    UINT count = 0;
    if (FAILED(hr = topology->GetConnectorCount(&count))) return hr;
    ComPtr<IKsControl> control;
    hr = E_NOINTERFACE; // an endpoint without connectors
    for (UINT i = 0; i < count && i < 16 && !control; ++i) {
        ComPtr<IConnector> connector, filterPin;
        ComPtr<IPart> part;
        if (FAILED(hr = topology->GetConnector(i, &connector)) || FAILED(hr = connector->GetConnectedTo(&filterPin)) ||
            FAILED(hr = filterPin.As(&part))) continue;
        hr = part->Activate(CLSCTX_INPROC_SERVER, __uuidof(IKsControl), reinterpret_cast<void **>(control.GetAddressOf()));
    }
    if (!control) return FAILED(hr) ? hr : E_NOINTERFACE;
    const BluetoothOneShotRequest request = BluetoothAudioOneShotRequest(connect);
    KSPROPERTY property = {};
    property.Set = request.set; property.Id = request.id; property.Flags = request.flags;
    ULONG returned = 0;
    at = OneShotStage::Driver;
    return control->KsProperty(&property, sizeof(property), nullptr, 0, &returned);
}

std::wstring BluetoothAudioOneShotText(HRESULT hr, OneShotStage stage, bool connect, bool pnDriver)
{
    const wchar_t *verb = connect ? L"connect" : L"disconnect";
    if (SUCCEEDED(hr)) return connect ? L"Connect request sent. The device shows as connected when the receiver answers."
                                      : L"Disconnect request sent.";
    std::wostringstream out;
    switch (stage) {
    case OneShotStage::NoEndpoint:
        out << L"Windows has no audio output for this device. If it does not come back, remove the device and pair it again.";
        break;
    case OneShotStage::Lookup:
        if (hr == E_INVALIDARG) out << L"No Bluetooth device is selected.";
        else if (hr == HRESULT_FROM_WIN32(ERROR_DUP_NAME)) out << L"Several Windows audio outputs match this device; the " << verb << L" request was not sent.";
        else out << L"Windows audio could not check its outputs; the " << verb << L" request was not sent. Retry in a moment.";
        break;
    case OneShotStage::Topology:
        out << L"The Windows audio output of this device could not be reached; the " << verb << L" request was not sent.";
        break;
    default:
        if (hr == HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION) || hr == HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND) ||
            hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
            out << L"This device's driver does not accept " << verb << L" requests from Windows.";
            if (pnDriver) out << L" PN-A2DP needs driver 0.1.0.21 or later.";
        } else if (hr == HRESULT_FROM_WIN32(ERROR_NOT_READY)) out << L"The driver is not ready (the device is starting or stopping). Retry in a moment.";
        else out << L"The driver rejected the " << verb << L" request.";
        break;
    }
    out << L" (0x" << std::hex << std::setw(8) << std::setfill(L'0') << static_cast<unsigned long>(hr) << L")";
    return out.str();
}

bool OneShotReconnectFallback(bool connect, OneShotStage stage, bool statusKnown, ULONG state)
{
    return connect && stage == OneShotStage::NoEndpoint && statusKnown &&
           (state == PNA2DP_STATE_IDLE || state == PNA2DP_STATE_ERROR);
}

std::wstring WindowsAudioFormat(const WindowsAudioInfo &info)
{
    if (FAILED(info.formatError)) return L"Not available";
    return std::to_wstring(info.sampleRate) + L" Hz, " + std::to_wstring(info.channels) + L" channels, " +
        std::to_wstring(info.bits) + (info.floatingPoint ? L"-bit float (Windows mix)" : L"-bit PCM (Windows mix)");
}
std::wstring WindowsAudioReport(const WindowsAudioInfo &info)
{
    std::wostringstream out;
    out << info.detail << L"\r\nOutput: " << (info.endpointName.empty() ? L"Unavailable" : info.endpointName)
        << L"\r\nEndpoint ID: " << info.endpointId << L"\r\nWindows mix format: " << WindowsAudioFormat(info)
        << L"\r\nDefault multimedia output: " << (!info.defaultKnown ? L"Unknown" : info.isDefault ? L"Yes" : L"No")
        << L"\r\nVolume: ";
    if (SUCCEEDED(info.volumeError)) out << std::lround(info.volume * 100) << L"%"; else out << L"Unavailable";
    out << L"\r\nMute: " << (FAILED(info.muteError) ? L"Unavailable" : info.muted ? L"On" : L"Off")
        << L"\r\nBluetooth codec / bitrate: not exposed by this Windows audio API.\r\n"
        << L"The Windows mix format is not the negotiated Bluetooth codec format.\r\n"
        << L"Query results (HRESULT): output=0x" << std::hex << static_cast<unsigned long>(info.error)
        << L", volume=0x" << static_cast<unsigned long>(info.volumeError)
        << L", mute=0x" << static_cast<unsigned long>(info.muteError)
        << L", format=0x" << static_cast<unsigned long>(info.formatError) << L"\r\n";
    return out.str();
}
}
