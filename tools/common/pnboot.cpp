/* Windows Test Mode management. SPDX-License-Identifier: MIT */
#include "pnboot.h"
#include "pnsigning.h"
#include <wbemidl.h>
#include <wrl/client.h>
#include <setupapi.h>
#include <stdio.h>
#pragma comment(lib, "wbemuuid.lib")
#pragma comment(lib, "oleaut32.lib")

namespace pn {
using Microsoft::WRL::ComPtr;
namespace {
struct Bstr {
    BSTR value;
    explicit Bstr(const wchar_t *s) : value(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(value); }
    operator BSTR() const { return value; }
};
struct Value {
    VARIANT v;
    Value() { VariantInit(&v); }
    ~Value() { VariantClear(&v); }
};
struct ComInit {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() { if (SUCCEEDED(result)) CoUninitialize(); }
};
// BCD's WMI provider requires impersonation and backup/restore privileges.
struct BcdPrivileges {
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES before[2]{};
    bool changed[2]{};
    BcdPrivileges() {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) return;
        const wchar_t *names[] = { SE_BACKUP_NAME, SE_RESTORE_NAME };
        for (int i = 0; i < 2; ++i) {
            TOKEN_PRIVILEGES p{}; p.PrivilegeCount = 1;
            if (!LookupPrivilegeValueW(nullptr, names[i], &p.Privileges[0].Luid)) continue;
            p.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            DWORD size = sizeof(before[i]);
            changed[i] = AdjustTokenPrivileges(token, FALSE, &p, size, &before[i], &size) && GetLastError() == ERROR_SUCCESS;
        }
    }
    ~BcdPrivileges() {
        if (!token) return;
        for (int i = 1; i >= 0; --i) if (changed[i]) AdjustTokenPrivileges(token, FALSE, &before[i], 0, nullptr, nullptr);
        CloseHandle(token);
    }
};

HRESULT MethodInput(IWbemServices *service, const wchar_t *type, const wchar_t *method, IWbemClassObject **input)
{
    ComPtr<IWbemClassObject> cls, definition;
    HRESULT hr = service->GetObject(Bstr(type), 0, nullptr, &cls, nullptr);
    if (SUCCEEDED(hr)) hr = cls->GetMethod(method, 0, &definition, nullptr);
    if (SUCCEEDED(hr)) hr = definition->SpawnInstance(0, input);
    return hr;
}
HRESULT EmbeddedResult(IWbemClassObject *output, const wchar_t *property, IWbemClassObject **object)
{
    Value result, embedded;
    HRESULT hr = output->Get(L"ReturnValue", 0, &result.v, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    if (result.v.vt != VT_BOOL) return E_UNEXPECTED;
    if (!result.v.boolVal) return S_FALSE; // unknown/not present, never assume disabled
    hr = output->Get(property, 0, &embedded.v, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    if (embedded.v.vt != VT_UNKNOWN || !embedded.v.punkVal) return E_UNEXPECTED;
    return embedded.v.punkVal->QueryInterface(IID_PPV_ARGS(object));
}
HRESULT ReadNextBoot(bool &enabled)
{
    if (!IsElevated()) return E_ACCESSDENIED;
    ComInit com;
    if (FAILED(com.result) && com.result != RPC_E_CHANGED_MODE) return com.result;
    HRESULT hr = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr);
    if (FAILED(hr) && hr != RPC_E_TOO_LATE) return hr;
    BcdPrivileges privileges;
    ComPtr<IWbemLocator> locator;
    ComPtr<IWbemServices> service;
    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator));
    if (SUCCEEDED(hr)) hr = locator->ConnectServer(Bstr(L"ROOT\\WMI"), nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &service);
    if (SUCCEEDED(hr)) hr = CoSetProxyBlanket(service.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
        RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
    if (FAILED(hr)) return hr;
    ComPtr<IWbemClassObject> input, output, object;
    hr = MethodInput(service.Get(), L"BcdStore", L"OpenObject", &input);
    Value id;
    id.v.vt = VT_BSTR;
    // Microsoft's well-known GUID for {current}; never change the default/other OS entry.
    id.v.bstrVal = SysAllocString(L"{fa926493-6f1c-4193-a414-58f0b2456d1e}");
    if (SUCCEEDED(hr)) hr = input->Put(L"Id", 0, &id.v, 0);
    if (SUCCEEDED(hr)) hr = service->ExecMethod(Bstr(L"BcdStore.FilePath=\"\""), Bstr(L"OpenObject"), 0, nullptr, input.Get(), &output, nullptr);
    if (FAILED(hr)) return hr;
    hr = EmbeddedResult(output.Get(), L"Object", &object);
    if (hr != S_OK) return hr == S_FALSE ? E_FAIL : hr;
    Value actualId;
    hr = object->Get(L"Id", 0, &actualId.v, nullptr, nullptr);
    if (FAILED(hr) || actualId.v.vt != VT_BSTR) return E_UNEXPECTED;
    CLSID guid{};
    if (FAILED(CLSIDFromString(actualId.v.bstrVal, &guid))) return E_UNEXPECTED;
    std::wstring path = L"BcdObject.Id=\"" + std::wstring(actualId.v.bstrVal) + L"\",StoreFilePath=\"\"";
    input.Reset(); output.Reset();
    hr = MethodInput(service.Get(), L"BcdObject", L"GetElement", &input);
    Value type;
    type.v.vt = VT_I4; type.v.lVal = 0x16000049; // AllowPrereleaseSignatures / testsigning
    if (SUCCEEDED(hr)) hr = input->Put(L"Type", 0, &type.v, 0);
    if (SUCCEEDED(hr)) hr = service->ExecMethod(Bstr(path.c_str()), Bstr(L"GetElement"), 0, nullptr, input.Get(), &output, nullptr);
    if (FAILED(hr)) return hr;
    ComPtr<IWbemClassObject> element;
    hr = EmbeddedResult(output.Get(), L"Element", &element);
    if (hr != S_OK) return hr;
    Value boolean;
    hr = element->Get(L"Boolean", 0, &boolean.v, nullptr, nullptr);
    if (FAILED(hr)) return hr;
    if (boolean.v.vt != VT_BOOL) return E_UNEXPECTED;
    enabled = boolean.v.boolVal != VARIANT_FALSE;
    return S_OK;
}
}

BootModeState QueryBootMode()
{
    BootModeState state;
    state.running = QuerySigningState();
    state.nextError = ReadNextBoot(state.nextTestSigning);
    return state;
}

BootDriverOperationLock::BootDriverOperationLock()
{
    handle = CreateMutexW(nullptr, FALSE, L"Local\\PN-A2DP.BootAndDriverOperation");
    if (!handle) { error = GetLastError(); return; }
    DWORD wait = WaitForSingleObject(handle, 0);
    owned = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    if (!owned) error = wait == WAIT_TIMEOUT ? ERROR_BUSY : GetLastError();
}
BootDriverOperationLock::~BootDriverOperationLock()
{
    if (owned) ReleaseMutex(handle);
    if (handle) CloseHandle(handle);
}

PackageTrust VerifyPnInf(const std::wstring &inf)
{
    wchar_t stored[MAX_PATH]{};
    std::wstring path = inf;
    if (SetupGetInfDriverStoreLocationW(inf.c_str(), nullptr, nullptr, stored, MAX_PATH, nullptr)) path = stored;
    if (_wcsicmp(BaseName(path).c_str(), L"pna2dp.inf")) {
        PackageTrust invalid; invalid.detail = L"The selected INF is not a PN-A2DP package."; return invalid;
    }
    auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    return VerifyDriverPackage(path.substr(0, separator));
}

DriverReadiness EvaluateDriverReadiness(const SigningState &state, const PackageTrust &trust)
{
    DriverReadiness result;
    auto block = [&](const wchar_t *reason) {
        result.error = ERROR_DRIVER_BLOCKED;
        if (!result.detail.empty()) result.detail += L"\r\n\r\n";
        result.detail += reason;
    };
    if (!trust.trusted) {
        block(L"The PN driver package could not be verified. Install a complete, trusted PN-A2DP package before activation.");
        if (!trust.detail.empty()) result.detail += L"\r\n" + trust.detail;
    } else if (!trust.microsoftDriver) {
        if (!state.secureBootKnown) block(L"Secure Boot status is unknown. Refresh and verify the firmware setting before activating the development driver.");
        else if (state.secureBoot) block(L"Secure Boot is ON. This development driver requires Secure Boot OFF. Windows mode can be used with Secure Boot enabled.");
        if (!state.queried) block(L"Windows did not return the current Test Mode / Memory Integrity state. Refresh before activation.");
        else {
            if (!state.testSigning) block(L"Test Mode is OFF in the current session. Enable it in Windows / Test Mode, close that window, then restart Windows manually before activating the development driver.");
            if (state.hvci) block(L"Memory Integrity is still running. This PN development build is validated with Memory Integrity OFF. Change it in Windows Security > Device security > Core isolation, then restart manually before activation.");
        }
        if (!state.hvciConfiguredKnown) block(L"The saved Memory Integrity setting is unknown. Check Windows Security and refresh before activation.");
        else if (state.hvciConfigured) block(L"Memory Integrity is configured ON. Turn it OFF in Windows Security and restart if requested before using this development build.");
    }
    if (result.ready()) result.detail = trust.microsoftDriver
        ? L"Microsoft driver signature verified. Test Mode and disabling security features are not required."
        : L"Development driver checks passed: Test Mode ON, Secure Boot OFF, Memory Integrity OFF. The next-boot setting is checked again with administrator permission before activation.";
    return result;
}

DriverReadiness EvaluateActivationReadiness(const BootModeState &state, const PackageTrust &trust)
{
    auto result = EvaluateDriverReadiness(state.running, trust);
    if (result.ready() && !trust.microsoftDriver && (state.nextError != S_OK || !state.nextTestSigning)) {
        result.error = ERROR_DRIVER_BLOCKED;
        result.detail = L"Test Mode is not confirmed ON for the next start. Open Windows / Test Mode, enable it and close the window. Restart manually if the current session is still normal mode, then retry activation.";
    }
    return result;
}

BootDriverDependencies QueryBootDriverDependencies()
{
    BootDriverDependencies out;
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    for (const auto &node : EnumerateA2dpSinkNodes(&out.error)) {
        if (!node.usesPnA2dp()) continue; // include disconnected/paired PN devices, too
        BootDriverDependency item{node.name.empty() ? node.instanceId : node.name, false};
        wchar_t stored[MAX_PATH]{};
        auto inf = JoinPath(JoinPath(windows, L"INF"), BaseName(node.driverInf));
        if (SetupGetInfDriverStoreLocationW(inf.c_str(), nullptr, nullptr, stored, MAX_PATH, nullptr)) {
            std::wstring dir(stored); dir.resize(dir.find_last_of(L"\\/"));
            auto trust = VerifyDriverPackage(dir);
            item.normalModeVerified = trust.trusted && trust.microsoftDriver;
        }
        out.devices.push_back(item);
    }
    return out;
}

std::wstring BootModeSummary(const BootModeState &state)
{
    std::wstring text = L"Current session: ";
    text += !state.running.queried ? L"unknown" : state.running.testSigning ? L"Test Mode ON" : L"Normal Windows mode (Test Mode OFF)";
    text += L"\r\nNext start of this Windows installation: ";
    if (state.nextError == S_OK) text += state.nextTestSigning ? L"Test Mode ON" : L"Normal Windows mode (Test Mode OFF)";
    else if (state.nextError == S_FALSE) text += L"not explicitly set; default/inherited setting";
    else { wchar_t code[32]; swprintf_s(code, L"0x%08X", (unsigned)state.nextError); text += L"unavailable (" + std::wstring(code) + L")"; }
    if (state.restartRequired()) text += L"\r\nRestart required. Save your work and restart Windows manually.";
    return text;
}

DWORD ChangeBootModeWithActions(bool enable, const BootModeActions &actions, std::wstring &detail)
{
    auto before = actions.read();
    if (FAILED(before.nextError) || !before.running.queried) {
        detail = L"Cannot read Windows boot settings. No change was made. Open this dialog with administrator permission and try Refresh.";
        return ERROR_ACCESS_DENIED;
    }
    if (enable && !before.running.secureBootKnown) {
        detail = L"Secure Boot status is unknown. No change was made. Refresh and verify the firmware setting first.";
        return ERROR_NOT_READY;
    }
    if (enable && before.running.secureBoot) {
        detail = L"Secure Boot is enabled and protects this setting. No change was made. You can use Windows mode without enabling Test Mode.";
        return ERROR_ACCESS_DISABLED_BY_POLICY;
    }
    if (!enable) {
        auto dependencies = actions.dependencies();
        if (dependencies.error != ERROR_SUCCESS) {
            detail = L"Windows could not enumerate the Bluetooth drivers, so Test Mode was not disabled. Click Refresh and try again.\r\n\r\n" + Win32ErrorText(dependencies.error);
            return dependencies.error;
        }
        std::wstring names;
        for (const auto &device : dependencies.devices) if (!device.normalModeVerified) names += L"\r\n- " + device.name;
        if (!names.empty()) {
            detail = L"These devices still use a PN-A2DP driver that is not verified for normal Windows mode:" + names +
                L"\r\n\r\nClose this dialog, select each device on the Driver tab and choose Use Windows mode. Then disable Test Mode. No boot setting was changed.";
            return ERROR_DRIVER_BLOCKED;
        }
    }
    if (before.nextError == S_OK && before.nextTestSigning == enable) {
        detail = L"That setting is already selected.\r\n\r\n" + BootModeSummary(before);
        return ERROR_SUCCESS;
    }
    DWORD error = actions.write(enable, detail);
    if (error) return error;
    auto after = actions.read();
    if (after.nextError != S_OK || after.nextTestSigning != enable) {
        detail = L"Windows accepted the command, but the saved boot setting could not be confirmed. Click Refresh and check it before restarting.\r\n\r\n" + BootModeSummary(after);
        return ERROR_WRITE_FAULT;
    }
    detail = L"Boot setting saved and verified.\r\n\r\n" + BootModeSummary(after);
    return ERROR_SUCCESS;
}

DWORD ChangeBootMode(bool enable, std::wstring &detail)
{
    if (!IsElevated()) { detail = L"Administrator permission is required. No change was made."; return ERROR_ACCESS_DENIED; }
    BootModeActions actions{ QueryBootMode, QueryBootDriverDependencies,
        [](bool value, std::wstring &text) { return SetTestSigning(value, &text); } };
    return ChangeBootModeWithActions(enable, actions, detail);
}
}
