/* Boot-mode transaction tests. No real boot writes or restarts. SPDX-License-Identifier: MIT */
#include "../pnboot.h"
#include <stdio.h>
#include <string.h>
#include <thread>

static int checks = 0, failures = 0;
static void Check(bool ok, const char *name) { ++checks; if (!ok) ++failures; printf("%s %s\n", ok ? "PASS" : "FAIL", name); }

struct FakeBoot {
    pn::BootModeState state;
    std::vector<pn::BootDriverDependency> devices;
    int writes = 0;
    DWORD writeError = 0;
    DWORD dependencyError = 0;
    bool corruptReadback = false;
    bool failReadback = false;
    FakeBoot() { state.running.queried = true; state.running.secureBootKnown = true; state.running.hvciConfiguredKnown = true; state.nextError = S_OK; }
    pn::BootModeActions actions() {
        return {
            [&] { return state; },
            [&] { return pn::BootDriverDependencies{dependencyError, devices}; },
            [&](bool enable, std::wstring &detail) {
                ++writes;
                if (writeError) { detail = L"Windows policy rejected the request"; return writeError; }
                if (!corruptReadback) state.nextTestSigning = enable;
                state.nextError = failReadback ? E_ACCESSDENIED : S_OK;
                return DWORD(ERROR_SUCCESS);
            }
        };
    }
};

int wmain(int argc, wchar_t **argv)
{
    bool probe = argc >= 2 && !wcscmp(argv[1], L"--probe");
    if (argc >= 3) { FILE *report = nullptr; if (_wfreopen_s(&report, argv[2], L"w", stdout)) return 2; }
    std::wstring detail;
    FakeBoot f;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == 0 && f.writes == 1, "enable writes and verifies next boot");
    Check(!f.state.running.testSigning && f.state.restartRequired(), "current session stays normal until reboot");
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == 0 && f.writes == 1, "duplicate enable is an idempotent no-op");
    Check(pn::ChangeBootModeWithActions(false, f.actions(), detail) == 0 && f.writes == 2 && !f.state.restartRequired(), "cancel pending enable restores next boot without restart");
    f.state.running.testSigning = true; f.state.nextTestSigning = true;
    f.devices = { {L"Disconnected headset", false} };
    Check(pn::ChangeBootModeWithActions(false, f.actions(), detail) == ERROR_DRIVER_BLOCKED && f.writes == 2, "test-signed PN device blocks disabling, even if disconnected");
    Check(detail.find(L"Disconnected headset") != std::wstring::npos && detail.find(L"Use Windows mode") != std::wstring::npos, "blocked device and recovery action are shown");
    f.devices[0].normalModeVerified = true;
    Check(pn::ChangeBootModeWithActions(false, f.actions(), detail) == 0 && f.state.restartRequired(), "verified Microsoft-signed PN driver permits normal mode");
    Check(f.state.running.testSigning, "disabling does not claim current session is already normal");
    f = FakeBoot(); f.state.nextTestSigning = true; f.dependencyError = ERROR_ACCESS_DENIED;
    Check(pn::ChangeBootModeWithActions(false, f.actions(), detail) == ERROR_ACCESS_DENIED && !f.writes, "failed device enumeration cannot silently permit disabling Test Mode");
    f = FakeBoot(); f.state.running.secureBoot = true;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == ERROR_ACCESS_DISABLED_BY_POLICY && f.writes == 0, "Secure Boot blocks enable without a write");
    f = FakeBoot(); f.state.running.secureBootKnown = false;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == ERROR_NOT_READY && !f.writes, "unknown Secure Boot cannot be treated as OFF");
    f = FakeBoot(); f.state.nextError = E_ACCESSDENIED;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) != 0 && f.writes == 0, "unreadable BCD blocks changes");
    Check(pn::BootModeSummary(f.state).find(L"unavailable") != std::wstring::npos, "unknown next boot is not displayed as disabled");
    f = FakeBoot(); f.state.nextError = S_FALSE;
    Check(pn::BootModeSummary(f.state).find(L"inherited") != std::wstring::npos && !f.state.restartRequired(), "missing explicit BCD element remains unknown");
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == 0 && f.writes == 1, "explicit setting can be created when absent");
    f = FakeBoot(); f.writeError = ERROR_ACCESS_DENIED;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == ERROR_ACCESS_DENIED && !f.state.restartRequired(), "OS rejection is preserved, without false restart success");
    Check(detail == L"Windows policy rejected the request", "OS rejection detail is preserved");
    f = FakeBoot(); f.corruptReadback = true;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == ERROR_WRITE_FAULT, "mismatched readback is not success");
    f = FakeBoot(); f.failReadback = true;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) == ERROR_WRITE_FAULT, "unavailable readback is not success");
    f = FakeBoot(); f.state.running.queried = false;
    Check(pn::ChangeBootModeWithActions(true, f.actions(), detail) != 0 && !f.writes, "unknown running signing state blocks changes");
    pn::PackageTrust trust; trust.trusted = true; trust.error = ERROR_SUCCESS;
    auto ready = FakeBoot().state;
    ready.running.testSigning = ready.nextTestSigning = true;
    Check(pn::EvaluateActivationReadiness(ready, trust).ready(), "verified development driver and ready platform permit activation");
    auto state = ready; state.running.secureBoot = true;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "Secure Boot ON blocks development activation");
    state = ready; state.running.secureBootKnown = false;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "unknown Secure Boot blocks development activation");
    state = ready; state.running.queried = false;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "unknown running integrity options block activation");
    state = ready; state.running.testSigning = false;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "pending Test Mode enable is not current Test Mode");
    state = ready; state.nextTestSigning = false;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "pending Test Mode disable blocks development activation");
    state = ready; state.nextError = E_ACCESSDENIED;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "unreadable next boot blocks elevated activation");
    Check(pn::EvaluateDriverReadiness(state.running, trust).ready(), "UI running-state check does not require administrator BCD access");
    state = ready; state.nextError = S_FALSE;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "inherited next-boot setting is not assumed to enable Test Mode");
    state = ready; state.running.hvci = true;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "HVCI running blocks development activation even when configured OFF");
    state = ready; state.running.hvciConfigured = true;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "HVCI configured ON blocks development activation before next boot");
    state = ready; state.running.hvciConfiguredKnown = false;
    Check(!pn::EvaluateActivationReadiness(state, trust).ready(), "unknown saved HVCI setting blocks development activation");
    state = ready; state.running.secureBoot = true; state.running.hvci = true; state.running.testSigning = false;
    auto reasons = pn::EvaluateActivationReadiness(state, trust).detail;
    Check(reasons.find(L"Secure Boot is ON") != std::wstring::npos && reasons.find(L"Memory Integrity is still running") != std::wstring::npos && reasons.find(L"Test Mode is OFF") != std::wstring::npos, "all failed requirements are explained together");
    trust.microsoftDriver = true;
    Check(pn::EvaluateActivationReadiness(state, trust).ready(), "Microsoft release signature does not require disabling platform protections");
    trust.trusted = false;
    Check(!pn::EvaluateActivationReadiness(ready, trust).ready(), "untrusted or damaged package is blocked even with Test Mode ON");
    Check(!pn::VerifyPnInf(L"not-a-pna2dp.inf").trusted, "unrelated INF cannot borrow a PN catalog verification");
    {
        pn::BootDriverOperationLock held;
        DWORD otherError = ERROR_SUCCESS;
        std::thread other([&] { pn::BootDriverOperationLock second; otherError = second.error; });
        other.join();
        Check(held.error == ERROR_SUCCESS && otherError == ERROR_BUSY, "boot dialog and driver operations cannot overlap");
    }
    { pn::BootDriverOperationLock released; Check(released.error == ERROR_SUCCESS, "closing a boot dialog releases the operation guard"); }
    auto real = pn::QueryBootMode();
    printf("%ls\n", pn::BootModeSummary(real).c_str());
    Check(real.running.queried, "real current-session signing state queried");
    printf("Secure Boot: known=%d on=%d; Memory Integrity: running=%d configured-known=%d configured=%d\n", real.running.secureBootKnown, real.running.secureBoot, real.running.hvci, real.running.hvciConfiguredKnown, real.running.hvciConfigured);
    if (probe) {
        Check(pn::IsElevated(), "read-only BCD probe is elevated");
        Check(real.nextError == S_OK || real.nextError == S_FALSE, "real BCD provider returns typed setting or absent element");
        auto dependencies = pn::QueryBootDriverDependencies();
        Check(dependencies.error == ERROR_SUCCESS, "real Bluetooth driver dependency query succeeded");
        for (const auto &d : dependencies.devices) printf("PN device: %ls, normal-mode verified=%d\n", d.name.c_str(), d.normalModeVerified);
        auto packages = pn::FindStagedPackages();
        if (!packages.empty()) {
            auto packageTrust = pn::VerifyPnInf(packages.back().fullPath);
            Check(packageTrust.trusted, "real selected PN package signature and file membership verified");
            auto activation = pn::EvaluateActivationReadiness(real, packageTrust);
            printf("PN activation ready=%d: %ls\n", activation.ready(), activation.detail.c_str());
        }
    } else if (!pn::IsElevated()) {
        Check(real.nextError == E_ACCESSDENIED, "non-admin query does not invent a next-boot value");
    }
    printf("%d checks, %d failures; no real boot settings changed.\n", checks, failures);
    return failures ? 1 : 0;
}
