/* Read-only discovery regression and optional silent object-stream test.
 * Does not select a renderer, endpoint, volume or license. SPDX-License-Identifier: MIT */
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <spatialaudioclient.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include "../common/pnctl.h"
#include "../common/pnspatial.h"
using Microsoft::WRL::ComPtr;
static unsigned checks;
static void Require(bool condition, const char *message) {
    ++checks; if (!condition) { fprintf(stderr, "FAIL: %s\n", message); throw 1; }
}
static void Check(HRESULT hr, const char *message) {
    if (FAILED(hr)) { fprintf(stderr, "%s: 0x%08lx\n", message, (unsigned long)hr); throw 1; }
}
static void RenderSilent(const std::wstring &id)
{
    ComPtr<IMMDeviceEnumerator> enumerator;
    Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator)), "Enumerator");
    ComPtr<IMMDevice> device;
    Check(enumerator->GetDevice(id.c_str(), &device), "Exact endpoint");
    ComPtr<ISpatialAudioClient> client;
    Check(device->Activate(__uuidof(ISpatialAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void **>(client.GetAddressOf())), "Spatial client");
    Check(client->IsSpatialAudioStreamAvailable(__uuidof(ISpatialAudioObjectRenderStream), nullptr), "Object renderer available");
    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = 1; format.nSamplesPerSec = 48000;
    format.wBitsPerSample = 32; format.nBlockAlign = 4; format.nAvgBytesPerSec = 192000;
    Check(client->IsAudioObjectFormatSupported(&format), "48 kHz float object format");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "Event creation");
    std::unique_ptr<void, decltype(&CloseHandle)> closeEvent(event, CloseHandle);
    SpatialAudioObjectRenderStreamActivationParams params = {};
    params.ObjectFormat = &format;
    params.StaticObjectTypeMask = static_cast<AudioObjectType>(AudioObjectType_FrontLeft | AudioObjectType_FrontRight);
    params.Category = AudioCategory_Media;
    params.EventHandle = event;
    PROPVARIANT activation = {};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE *>(&params);
    ComPtr<ISpatialAudioObjectRenderStream> stream;
    Check(client->ActivateSpatialAudioStream(&activation, __uuidof(ISpatialAudioObjectRenderStream), reinterpret_cast<void **>(stream.GetAddressOf())), "Activate object stream");
    ComPtr<ISpatialAudioObject> left, right;
    Check(stream->ActivateSpatialAudioObject(AudioObjectType_FrontLeft, &left), "Left object");
    Check(stream->ActivateSpatialAudioObject(AudioObjectType_FrontRight, &right), "Right object");
    Check(stream->Start(), "Start spatial stream");
    struct Stop { ISpatialAudioObjectRenderStream *stream; ~Stop() { stream->Stop(); } } stop{stream.Get()};
    ULONGLONG end = GetTickCount64() + 5000;
    unsigned updates = 0; UINT64 total = 0;
    while (GetTickCount64() < end) {
        Require(WaitForSingleObject(event, 2000) == WAIT_OBJECT_0, "Spatial event within two seconds");
        UINT32 available = 0, frames = 0;
        Check(stream->BeginUpdatingAudioObjects(&available, &frames), "Begin spatial update");
        for (auto object : {left.Get(), right.Get()}) {
            BYTE *buffer = nullptr; UINT32 bytes = 0;
            Check(object->GetBuffer(&buffer, &bytes), "Object buffer");
            Require(buffer != nullptr && bytes >= static_cast<UINT64>(frames) * format.nBlockAlign, "Object buffer covers frames");
            memset(buffer, 0, bytes);
        }
        Check(stream->EndUpdatingAudioObjects(), "End spatial update");
        total += frames; ++updates;
    }
    Require(total > 0 && updates > 1, "Spatial rendering advanced");
    printf("Silent spatial stream: %u updates, %llu frames, %lu Hz.\n", updates, total, format.nSamplesPerSec);
}
int main(int argc, char **argv)
{
    bool requireLive = argc > 1 && (!strcmp(argv[1], "--require-live") || !strcmp(argv[1], "--render-silent"));
    bool render = argc > 1 && !strcmp(argv[1], "--render-silent");
    HRESULT apartment = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;
    try {
        pn::SpatialInfo unknown;
        Require(pn::SpatialActiveName(unknown) == L"Unavailable", "Unknown must not claim Off");
        Require(pn::SpatialSupportText(unknown, pn::SpatialFormat::AtmosHeadphones) == L"Not detected", "Query failure must not claim unsupported");
        unknown.queried = true;
        unknown.formats[3].support = pn::SpatialSupport::Supported;
        Require(pn::SpatialSupportText(unknown, pn::SpatialFormat::AtmosTheater) == L"Unavailable over A2DP", "Provider cannot override A2DP transport limit");
        auto missing = pn::QuerySpatialAudio(L"BTHENUM\\PN_A2DP_NONEXISTENT_TEST_DEVICE");
        Require(!missing.queried && missing.endpointId.empty(), "Missing device must not fall back to default output");
        Require(!pn::QuerySpatialAudio(L"").queried, "Empty selection must not query default output");
        unsigned live = 0;
        for (const auto &iface : pn::EnumerateControlInterfaces()) {
            auto info = pn::QuerySpatialAudio(iface.instanceId);
            if (!info.queried) { printf("No active/queryable output: %ls\n", info.detail.c_str()); continue; }
            ++live;
            Require(!info.interfaceId.empty() && info.interfaceId != info.endpointId, "Use WinRT interface ID, not MMDevice ID");
            Require(info.endpointState == DEVICE_STATE_ACTIVE, "Do not pick cached old-driver endpoint");
            Require(!info.activeFormat.empty() && !info.selectedFormat.empty(), "Read active and selected independently");
            pn::SpatialInfo mta;
            std::thread worker([&] { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); mta = pn::QuerySpatialAudio(iface.instanceId); if (SUCCEEDED(hr)) CoUninitialize(); });
            worker.join();
            Require(mta.queried && mta.endpointId == info.endpointId, "STA and MTA query same physical output");
            printf("LIVE: %ls, active=%ls, selected=%ls\n", info.endpointName.c_str(), pn::SpatialActiveName(info).c_str(), pn::SpatialActiveName(info, true).c_str());
            if (render) RenderSilent(info.endpointId);
        }
        Require(!requireLive || live > 0, "Requested live PN-A2DP output exists");
        printf("PASS: %u checks; %u live outputs; no settings changed.\n", checks, live);
    } catch (...) { result = 1; }
    if (SUCCEEDED(apartment)) CoUninitialize();
    return result;
}
