/* Windows backend integration tests; live writes require explicit --exercise. SPDX-License-Identifier: MIT */
#include "../common/pnaudio.h"
#include "../common/pndrv.h"
#include <stdio.h>
#include <objbase.h>
#include <limits>
#include <cmath>
#include <thread>

static unsigned failures = 0, checks = 0;
static void Check(bool ok, const char *name)
{ ++checks; printf("%s %s\n", ok ? "PASS" : "FAIL", name); if (!ok) ++failures; }

int wmain(int argc, wchar_t **argv)
{
    ULONGLONG address = 0;
    bool exercise = false, requireInbox = false;
    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--device") && i + 1 < argc) {
            wchar_t *end = nullptr; address = _wcstoui64(argv[++i], &end, 16);
            if (!address || *end || address > 0xffffffffffffull) return 2;
        } else if (!wcscmp(argv[i], L"--exercise")) exercise = true;
        else if (!wcscmp(argv[i], L"--require-inbox")) requireInbox = true;
        else return 2;
    }
    if ((exercise || requireInbox) && !address) return 2;
    Check(pn::QueryWindowsAudio(L"").error == E_INVALIDARG, "empty identity rejected");
    const std::wstring missing = L"BTHENUM\\PN_A2DP_TEST_MISSING_DEVICE";
    auto absent = pn::QueryWindowsAudio(missing);
    Check(!absent.active && absent.endpointId.empty() && FAILED(absent.volumeError), "missing device never falls back to default audio");
    Check(FAILED(pn::SetWindowsMute(missing, L"missing", true)), "missing-device mute fails closed");
    Check(pn::SetWindowsVolume(missing, L"missing", -0.01f) == E_INVALIDARG, "negative volume rejected");
    Check(pn::SetWindowsVolume(missing, L"missing", 1.01f) == E_INVALIDARG, "volume above 100 percent rejected");
    Check(pn::SetWindowsVolume(missing, L"missing", std::numeric_limits<float>::quiet_NaN()) == E_INVALIDARG, "NaN rejected");
    Check(pn::SetWindowsVolume(missing, L"missing", std::numeric_limits<float>::infinity()) == E_INVALIDARG, "infinity rejected");
    Check(pn::SetWindowsVolume(missing, L"", 0.1f) == E_INVALIDARG, "empty endpoint identity rejected");
    auto nodes = pn::EnumerateA2dpSinkNodes();
    bool found = false;
    for (const auto &node : nodes) {
        if (address && node.address != address) continue;
        if (address && found) { Check(false, "unique requested device"); break; }
        found = true;
        auto info = pn::QueryWindowsAudio(node.instanceId);
        printf("DEVICE %ls (%012llX), service %ls\n%ls", node.name.c_str(), node.address, node.service.c_str(), pn::WindowsAudioReport(info).c_str());
        if (requireInbox) { Check(node.usesWindowsA2dp(), "Microsoft inbox driver configured"); Check(!node.restartRequired, "driver change completed without pending restart"); }
        if (address) Check(info.active, "target has an active output");
        if (!info.active) continue;
        Check(!info.endpointId.empty() && SUCCEEDED(info.error), "active endpoint has an exact identity");
        Check(SUCCEEDED(info.volumeError) && info.volume >= 0 && info.volume <= 1, "live volume readable");
        Check(SUCCEEDED(info.muteError), "live mute readable");
        Check(SUCCEEDED(info.formatError) && info.sampleRate && info.channels, "Windows mix format readable");
        Check(pn::SetWindowsVolume(node.instanceId, info.endpointId + L"-stale", 0) == HRESULT_FROM_WIN32(ERROR_RETRY), "stale slider target rejected");
        Check(pn::SetWindowsMute(node.instanceId, info.endpointId + L"-stale", true) == HRESULT_FROM_WIN32(ERROR_RETRY), "stale mute target rejected");
        pn::WindowsAudioInfo mta;
        std::thread thread([&] { HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); mta = pn::QueryWindowsAudio(node.instanceId); if (SUCCEEDED(hr)) CoUninitialize(); });
        thread.join();
        Check(mta.active && mta.endpointId == info.endpointId, "MTA and STA resolve the same output");
        auto afterInvalid = pn::QueryWindowsAudio(node.instanceId);
        Check(std::fabs(afterInvalid.volume - info.volume) < 0.002f && afterInvalid.muted == info.muted, "rejected writes leave selected output unchanged");
        if (exercise && SUCCEEDED(info.volumeError) && SUCCEEDED(info.muteError)) {
            struct Restore {
                const std::wstring &instance; const pn::WindowsAudioInfo &saved;
                ~Restore() {
                    Check(SUCCEEDED(pn::SetWindowsVolume(instance, saved.endpointId, saved.volume)), "restore original volume");
                    Check(SUCCEEDED(pn::SetWindowsMute(instance, saved.endpointId, saved.muted)), "restore original mute");
                }
            } restore{node.instanceId, info};
            Check(SUCCEEDED(pn::SetWindowsMute(node.instanceId, info.endpointId, true)), "set mute on selected output");
            Check(pn::QueryWindowsAudio(node.instanceId).muted, "mute reads back");
            float lower = info.volume * 0.5f;
            Check(SUCCEEDED(pn::SetWindowsVolume(node.instanceId, info.endpointId, lower)), "lower selected output volume");
            auto lowered = pn::QueryWindowsAudio(node.instanceId);
            Check(SUCCEEDED(lowered.volumeError) && std::fabs(lowered.volume - lower) < 0.025f, "lower volume reads back (allows hardware steps)");
        }
    }
    if (address) Check(found, "requested Bluetooth device found");
    printf("%u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
