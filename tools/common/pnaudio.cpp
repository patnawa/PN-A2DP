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
bool EndpointMatches(IMMDeviceEnumerator *enumerator, IMMDevice *endpoint, const std::wstring &ancestor)
{
    // Endpoint property stores do not consistently expose PKEY_Device_InstanceId.
    // Follow the documented topology connector to the actual audio adapter.
    ComPtr<IDeviceTopology> topology;
    if (FAILED(endpoint->Activate(__uuidof(IDeviceTopology), CLSCTX_INPROC_SERVER, nullptr,
            reinterpret_cast<void **>(topology.GetAddressOf())))) return false;
    UINT count = 0;
    if (FAILED(topology->GetConnectorCount(&count))) return false;
    for (UINT i = 0; i < count && i < 16; ++i) {
        ComPtr<IConnector> connector;
        if (FAILED(topology->GetConnector(i, &connector))) continue;
        LPWSTR rawId = nullptr;
        if (FAILED(connector->GetDeviceIdConnectedTo(&rawId))) continue;
        std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> freeId(rawId, CoTaskMemFree);
        ComPtr<IMMDevice> adapter;
        ComPtr<IPropertyStore> properties;
        if (FAILED(enumerator->GetDevice(rawId, &adapter)) || FAILED(adapter->OpenPropertyStore(STGM_READ, &properties))) continue;
        if (DescendsFrom(PropertyString(properties.Get(), PKEY_Device_InstanceId), ancestor)) return true;
    }
    return false;
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


HRESULT FindOutput(const std::wstring &ancestor, WindowsAudioInfo &info, ComPtr<IMMDevice> &output)
{
    if (ancestor.empty()) return E_INVALIDARG;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return hr;
    UINT count = 0;
    if (FAILED(hr = devices->GetCount(&count))) return hr;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> candidate;
        if (FAILED(devices->Item(i, &candidate)) || !EndpointMatches(enumerator.Get(), candidate.Get(), ancestor)) continue;
        if (output) { output.Reset(); return HRESULT_FROM_WIN32(ERROR_DUP_NAME); }
        output = candidate;
    }
    if (!output) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
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

WindowsAudioInfo QueryWindowsAudio(const std::wstring &instance)
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

HRESULT SetWindowsVolume(const std::wstring &instance, const std::wstring &expected, float scalar)
{ return ChangeVolume(instance, expected, &scalar, nullptr); }
HRESULT SetWindowsMute(const std::wstring &instance, const std::wstring &expected, bool muted)
{ return ChangeVolume(instance, expected, nullptr, &muted); }

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
