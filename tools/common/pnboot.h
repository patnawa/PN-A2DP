/* Windows Test Mode management. SPDX-License-Identifier: MIT */
#pragma once
#include "pndrv.h"
#include "pnsigning.h"
#include <functional>

namespace pn {
struct BootModeState {
    SigningState running;
    HRESULT nextError = E_PENDING;
    bool nextTestSigning = false;
    bool restartRequired() const { return running.queried && nextError == S_OK && running.testSigning != nextTestSigning; }
};
struct BootDriverDependency {
    std::wstring name;
    bool normalModeVerified = false;
};
struct BootDriverDependencies {
    DWORD error = ERROR_SUCCESS;
    std::vector<BootDriverDependency> devices;
};
BootModeState QueryBootMode();
BootDriverDependencies QueryBootDriverDependencies();
std::wstring BootModeSummary(const BootModeState &state);

// The UI checks running state; the elevated helper also checks the next boot.
// Development packages are gated to the platform configuration validated for PN.
struct DriverReadiness {
    DWORD error = ERROR_SUCCESS;
    std::wstring detail;
    bool ready() const { return error == ERROR_SUCCESS; }
};
PackageTrust VerifyPnInf(const std::wstring &inf);
DriverReadiness EvaluateDriverReadiness(const SigningState &state, const PackageTrust &trust);
DriverReadiness EvaluateActivationReadiness(const BootModeState &state, const PackageTrust &trust);

// Serializes the boot dialog and driver changes, including separate elevated helpers.
class BootDriverOperationLock {
    HANDLE handle = nullptr;
    bool owned = false;
public:
    DWORD error = ERROR_SUCCESS;
    BootDriverOperationLock();
    ~BootDriverOperationLock();
    BootDriverOperationLock(const BootDriverOperationLock &) = delete;
    BootDriverOperationLock &operator=(const BootDriverOperationLock &) = delete;
};

// The same transaction is exercised with a fake backend in tests; it never restarts
// Windows, changes Secure Boot/BitLocker/HVCI, imports certificates or binds drivers.
struct BootModeActions {
    std::function<BootModeState()> read;
    std::function<BootDriverDependencies()> dependencies;
    std::function<DWORD(bool, std::wstring &)> write;
};
DWORD ChangeBootModeWithActions(bool enable, const BootModeActions &actions, std::wstring &detail);
DWORD ChangeBootMode(bool enable, std::wstring &detail);
}
