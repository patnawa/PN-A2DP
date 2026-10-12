/* Explicit-endpoint WASAPI bring-up tool. No default-device or volume changes.
 * SPDX-License-Identifier: MIT */
#define NOMINMAX
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ks.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

using Microsoft::WRL::ComPtr;
struct Failure { const char *operation; HRESULT hr; };
static void Check(HRESULT hr, const char *operation) { if (FAILED(hr)) throw Failure{operation, hr}; }
struct TaskFree { void operator()(void *p) const { CoTaskMemFree(p); } };
struct HandleClose { void operator()(void *p) const { if (p) CloseHandle(p); } };
struct StopStream { IAudioClient *client; ~StopStream() { client->Stop(); } };
struct RestoreVolume {
    ComPtr<IAudioEndpointVolume> control;
    float db = 0, minimum = 0;
    BOOL muted = FALSE;
    bool saved = false;
    ~RestoreVolume() { if (saved) { control->SetMasterVolumeLevel(db, nullptr); control->SetMute(muted, nullptr); } }
};

static void List(IMMDeviceEnumerator *enumerator)
{
    ComPtr<IMMDeviceCollection> devices;
    Check(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &devices), "EnumAudioEndpoints");
    UINT count = 0;
    Check(devices->GetCount(&count), "GetCount");
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        Check(devices->Item(i, &device), "Item");
        LPWSTR rawId = nullptr;
        Check(device->GetId(&rawId), "GetId");
        std::unique_ptr<wchar_t, TaskFree> id(rawId);
        DWORD state = 0;
        Check(device->GetState(&state), "GetState");
        ComPtr<IPropertyStore> props;
        Check(device->OpenPropertyStore(STGM_READ, &props), "OpenPropertyStore");
        PROPVARIANT name;
        PropVariantInit(&name);
        HRESULT hr = props->GetValue(PKEY_Device_FriendlyName, &name);
        printf("%s | %ls\n  %ls\n", state == DEVICE_STATE_ACTIVE ? "ACTIVE" :
               state == DEVICE_STATE_DISABLED ? "DISABLED" : state == DEVICE_STATE_UNPLUGGED ? "UNPLUGGED" : "NOT PRESENT",
               SUCCEEDED(hr) && name.vt == VT_LPWSTR ? name.pwszVal : L"(unnamed)", id.get());
        PropVariantClear(&name);
    }
    printf("%u render endpoints; no audio played.\n", count);
}

static void Play(IMMDeviceEnumerator *enumerator, const wchar_t *id, unsigned seconds, bool silent, bool volumeTest)
{
    ComPtr<IMMDevice> device;
    Check(enumerator->GetDevice(id, &device), "GetDevice (exact endpoint ID)");
    DWORD state = 0;
    Check(device->GetState(&state), "GetState");
    if (state != DEVICE_STATE_ACTIVE) throw Failure{"Endpoint is not active", AUDCLNT_E_DEVICE_INVALIDATED};
    ComPtr<IMMEndpoint> endpoint;
    Check(device.As(&endpoint), "IMMEndpoint");
    EDataFlow flow;
    Check(endpoint->GetDataFlow(&flow), "GetDataFlow");
    if (flow != eRender) throw Failure{"Endpoint is not an output", E_INVALIDARG};
    RestoreVolume volume;
    if (volumeTest) {
        Check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
            (void **)volume.control.GetAddressOf()), "IAudioEndpointVolume");
        Check(volume.control->GetMasterVolumeLevel(&volume.db), "Save volume");
        Check(volume.control->GetMute(&volume.muted), "Save mute");
        float maximum, step;
        Check(volume.control->GetVolumeRange(&volume.minimum, &maximum, &step), "Volume range");
        volume.saved = true;
        printf("Volume test: 3s original (%.1f dB), 2s muted, 2s quieter, 3s original; settings restored on exit.\n", volume.db);
    }
    ComPtr<IAudioClient> client;
    Check(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)client.GetAddressOf()), "Activate");
    WAVEFORMATEX *rawFormat = nullptr;
    Check(client->GetMixFormat(&rawFormat), "GetMixFormat");
    std::unique_ptr<WAVEFORMATEX, TaskFree> format(rawFormat);
    WAVEFORMATEX *f = format.get();
    bool floating = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool integer = f->wFormatTag == WAVE_FORMAT_PCM;
    unsigned validBits = f->wBitsPerSample;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        auto *ext = (WAVEFORMATEXTENSIBLE *)f;
        floating = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
        integer = IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
        validBits = ext->Samples.wValidBitsPerSample;
    }
    if ((!floating && !integer) || !f->nChannels || !f->nSamplesPerSec ||
        (floating && f->wBitsPerSample != 32) ||
        (integer && f->wBitsPerSample != 16 && f->wBitsPerSample != 24 && f->wBitsPerSample != 32) ||
        !validBits || validBits > f->wBitsPerSample ||
        f->nBlockAlign != f->nChannels * (f->wBitsPerSample / 8)) {
        throw Failure{"Unsupported mix format", AUDCLNT_E_UNSUPPORTED_FORMAT};
    }
    std::unique_ptr<void, HandleClose> event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event) throw Failure{"CreateEvent", HRESULT_FROM_WIN32(GetLastError())};
    Check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                             0, 0, f, nullptr), "Initialize");
    Check(client->SetEventHandle(event.get()), "SetEventHandle");
    UINT32 bufferFrames = 0;
    Check(client->GetBufferSize(&bufferFrames), "GetBufferSize");
    if (!bufferFrames) throw Failure{"Zero-sized endpoint buffer", E_UNEXPECTED};
    ComPtr<IAudioRenderClient> render;
    Check(client->GetService(__uuidof(IAudioRenderClient), (void **)render.GetAddressOf()), "IAudioRenderClient");
    ComPtr<IAudioClock> clock;
    Check(client->GetService(__uuidof(IAudioClock), (void **)clock.GetAddressOf()), "IAudioClock");
    UINT64 frequency = 0, initialPosition = 0, finalPosition = 0;
    Check(clock->GetFrequency(&frequency), "Clock frequency");
    if (!frequency) throw Failure{"Zero clock frequency", E_UNEXPECTED};
    Check(clock->GetPosition(&initialPosition, nullptr), "Initial clock position");

    const uint64_t totalFrames = uint64_t(seconds) * f->nSamplesPerSec;
    uint64_t submitted = 0;
    auto fill = [&](UINT32 frames) {
        BYTE *data = nullptr;
        Check(render->GetBuffer(frames, &data), "GetBuffer");
        if (!silent) {
            for (UINT32 frame = 0; frame < frames; ++frame) {
                uint64_t position = submitted + frame;
                double ramp = std::min(1.0, double(std::min(position, totalFrames - position)) / (f->nSamplesPerSec * 0.02));
                for (unsigned channel = 0; channel < f->nChannels; ++channel) {
                    double v = 0.03 * ramp * std::sin(6.283185307179586 * (channel % 2 ? 660.0 : 440.0) * double(position) / f->nSamplesPerSec);
                    BYTE *sample = data + size_t(frame) * f->nBlockAlign + channel * (f->wBitsPerSample / 8);
                    if (floating) { float value = (float)v; memcpy(sample, &value, sizeof(value)); }
                    else {
                        int32_t value = (int32_t)(v * 2147483647.0);
                        uint32_t bits = (uint32_t)value;
                        if (validBits < 32) bits &= ~((uint32_t(1) << (32 - validBits)) - 1);
                        bits >>= 32 - f->wBitsPerSample;
                        for (unsigned byte = 0; byte < unsigned(f->wBitsPerSample) / 8; ++byte) sample[byte] = (BYTE)(bits >> (byte * 8));
                    }
                }
            }
        }
        Check(render->ReleaseBuffer(frames, silent ? AUDCLNT_BUFFERFLAGS_SILENT : 0), "ReleaseBuffer");
        submitted += frames;
    };
    printf("Endpoint: %ls\nFormat: %u Hz, %u channels, %u/%u-bit %s; buffer %u frames; %u seconds %s\n",
           id, f->nSamplesPerSec, unsigned(f->nChannels), validBits, unsigned(f->wBitsPerSample),
           floating ? "float" : "PCM", bufferFrames, seconds, silent ? "silence" : "quiet test tone");
    fill((UINT32)std::min(uint64_t(bufferFrames), totalFrames));
    Check(client->Start(), "Start");
    StopStream stop{client.Get()};
    ULONGLONG started = GetTickCount64();
    UINT32 padding = 0;
    unsigned wakeups = 0, emptyDuringFill = 0;
    unsigned volumePhase = 0;
    for (;;) {
        DWORD wait = WaitForSingleObject(event.get(), 2000);
        if (wait != WAIT_OBJECT_0) throw Failure{"Audio event wait", HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError())};
        ++wakeups;
        if (volumeTest) {
            ULONGLONG elapsed = GetTickCount64() - started;
            unsigned phase = elapsed >= 7000 ? 3 : elapsed >= 5000 ? 2 : elapsed >= 3000 ? 1 : 0;
            if (phase != volumePhase) {
                float db = phase == 2 ? std::max(volume.minimum, volume.db - 12.0f) : volume.db;
                Check(volume.control->SetMasterVolumeLevel(db, nullptr), "Set test volume");
                Check(volume.control->SetMute(phase == 1 ? TRUE : volume.muted, nullptr), "Set test mute");
                float actualDb; BOOL actualMute;
                Check(volume.control->GetMasterVolumeLevel(&actualDb), "Read test volume");
                Check(volume.control->GetMute(&actualMute), "Read test mute");
                if (std::abs(actualDb - db) > 0.51f || !!actualMute != !!(phase == 1 ? TRUE : volume.muted))
                    throw Failure{"Volume/mute readback mismatch", E_UNEXPECTED};
                printf("Volume phase %u: %.1f dB, mute=%d\n", phase, actualDb, actualMute);
                volumePhase = phase;
            }
        }
        Check(client->GetCurrentPadding(&padding), "GetCurrentPadding");
        if (padding > bufferFrames) throw Failure{"Invalid endpoint padding", E_UNEXPECTED};
        if (submitted == totalFrames && padding == 0) break;
        if (submitted < totalFrames) {
            if (!padding) ++emptyDuringFill;
            UINT32 frames = (UINT32)std::min(uint64_t(bufferFrames - padding), totalFrames - submitted);
            if (frames) fill(frames);
        }
        if (GetTickCount64() - started > uint64_t(seconds + 5) * 1000) throw Failure{"Playback did not drain", HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
    }
    // Shared-mode padding can reach zero before the device clock reaches the last
    // submitted frame. Keep the stream running while those frames leave the engine.
    ULONGLONG drainDeadline = GetTickCount64() + 2000;
    for (;;) {
        Check(clock->GetPosition(&finalPosition, nullptr), "Final clock position");
        if (finalPosition >= initialPosition &&
            double(finalPosition - initialPosition) * f->nSamplesPerSec / double(frequency) >= double(totalFrames)) break;
        if (GetTickCount64() >= drainDeadline) throw Failure{"Device clock did not drain", HRESULT_FROM_WIN32(ERROR_TIMEOUT)};
        Sleep(5);
    }
    double elapsedFrames = double(finalPosition - initialPosition) * f->nSamplesPerSec / double(frequency);
    printf("Submitted %llu frames; clock advanced %.0f frames; %u events; %u empty-buffer observations while feeding.\n",
           (unsigned long long)submitted, elapsedFrames, wakeups, emptyDuringFill);
    if (finalPosition < initialPosition || elapsedFrames < double(totalFrames)) throw Failure{"Audio clock did not advance as expected", E_UNEXPECTED};
    Check(client->Stop(), "Stop");
    printf("PASS: WASAPI accepted and drained the stream. Confirm audible output and PN-A2DP packet counters separately.\n");
}

static void InspectVolume(IMMDeviceEnumerator *enumerator, const wchar_t *id)
{
    ComPtr<IMMDevice> device;
    Check(enumerator->GetDevice(id, &device), "GetDevice");
    ComPtr<IAudioEndpointVolume> volume;
    Check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)volume.GetAddressOf()), "EndpointVolume");
    float db, scalar, minimum, maximum, step; BOOL muted; DWORD hardware;
    Check(volume->GetMasterVolumeLevel(&db), "GetMasterVolumeLevel");
    Check(volume->GetMasterVolumeLevelScalar(&scalar), "GetMasterVolumeLevelScalar");
    Check(volume->GetMute(&muted), "GetMute");
    Check(volume->GetVolumeRange(&minimum, &maximum, &step), "GetVolumeRange");
    Check(volume->QueryHardwareSupport(&hardware), "QueryHardwareSupport");
    printf("Endpoint volume %.3f dB (%.1f%%), mute=%d; range %.1f..%.1f dB step %.1f; hardware flags 0x%lx\n",
        db, scalar * 100, muted, minimum, maximum, step, hardware);
    ComPtr<IAudioMeterInformation> meter;
    Check(device->Activate(__uuidof(IAudioMeterInformation), CLSCTX_ALL, nullptr, (void**)meter.GetAddressOf()), "AudioMeter");
    float peak = 0;
    for (int i = 0; i < 20; ++i) { float sample; Check(meter->GetPeakValue(&sample), "GetPeakValue"); peak = std::max(peak, sample); Sleep(50); }
    printf("Audio engine peak over 1 second: %.6f (%.1f dBFS); no settings changed.\n", peak, 20 * std::log10(std::max(peak, 0.0000001f)));
}

int wmain(int argc, wchar_t **argv)
{
    const wchar_t *endpoint = nullptr;
    unsigned seconds = 5;
    bool list = argc == 1, silent = false, volumeTest = false, volumeInfo = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--list")) list = true;
        else if (!wcscmp(argv[i], L"--play") && i + 1 < argc) endpoint = argv[++i];
        else if (!wcscmp(argv[i], L"--volume-info") && i + 1 < argc) { endpoint = argv[++i]; volumeInfo = true; }
        else if (!wcscmp(argv[i], L"--silent")) silent = true;
        else if (!wcscmp(argv[i], L"--volume-test")) volumeTest = true;
        else if (!wcscmp(argv[i], L"--seconds") && i + 1 < argc) {
            wchar_t *end = nullptr;
            unsigned long value = wcstoul(argv[++i], &end, 10);
            if (*end || value < 1 || value > 30) { fprintf(stderr, "Seconds must be 1..30.\n"); return 2; }
            seconds = (unsigned)value;
        } else { fprintf(stderr, "Usage: audio_test --list | --play <exact endpoint ID> [--seconds 1..30] [--silent] [--volume-test]\n"); return 2; }
    }
    if ((!list && !endpoint) || (list && endpoint)) { fprintf(stderr, "Select either --list or --play <ID>.\n"); return 2; }
    if (volumeTest && (list || silent)) { fprintf(stderr, "--volume-test requires audible --play.\n"); return 2; }
    if (volumeTest) seconds = 10;
    HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized)) { fprintf(stderr, "CoInitializeEx: 0x%08lx\n", (unsigned long)initialized); return 1; }
    int result = 0;
    try {
        ComPtr<IMMDeviceEnumerator> enumerator;
        Check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)), "MMDeviceEnumerator");
        if (list) List(enumerator.Get()); else if (volumeInfo) InspectVolume(enumerator.Get(), endpoint); else Play(enumerator.Get(), endpoint, seconds, silent, volumeTest);
    } catch (const Failure &f) {
        fprintf(stderr, "FAIL: %s: 0x%08lx\n", f.operation, (unsigned long)f.hr);
        result = 1;
    }
    CoUninitialize();
    return result;
}
