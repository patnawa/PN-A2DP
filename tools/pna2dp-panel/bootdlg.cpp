/* SPDX-License-Identifier: MIT */
#include "bootdlg.h"
#include "resource.h"
#include "pnboot.h"
#include <commctrl.h>

namespace {
struct DialogState { bool preview = false; bool busy = false; pn::BootModeState boot; };

void Refresh(HWND window, DialogState &state)
{
    state.busy = true;
    state.boot = pn::QueryBootMode();
    SetDlgItemTextW(window, IDC_BOOT_STATE, pn::BootModeSummary(state.boot).c_str());
    std::wstring platform = L"Secure Boot: ";
    platform += !state.boot.running.secureBootKnown ? L"Unknown" : state.boot.running.secureBoot ? L"ON" : L"OFF";
    platform += L"   |   Memory Integrity (running): ";
    platform += !state.boot.running.queried ? L"Unknown" : state.boot.running.hvci ? L"ON" : L"OFF";
    SetDlgItemTextW(window, IDC_BOOT_PLATFORM, platform.c_str());
    std::wstring dependency;
    auto dependencies = pn::QueryBootDriverDependencies();
    for (const auto &device : dependencies.devices) if (!device.normalModeVerified)
        dependency += L"\r\n- " + device.name;
    std::wstring guidance = dependency.empty()
        ? L"No paired device depends on a test-signed PN-A2DP driver. Other test-signed drivers may still require Test Mode."
        : L"Before turning Test Mode OFF, use the Driver tab to switch these devices to Windows mode:" + dependency;
    if (dependencies.error) guidance = L"Windows could not check the Bluetooth drivers. Refresh before disabling Test Mode.\r\n" + pn::Win32ErrorText(dependencies.error);
    SetDlgItemTextW(window, IDC_BOOT_DEVICES, guidance.c_str());
    bool canChange = !state.preview && pn::IsElevated() && !FAILED(state.boot.nextError) && state.boot.running.queried;
    EnableWindow(GetDlgItem(window, IDC_BOOT_ENABLE), canChange && state.boot.running.secureBootKnown && !state.boot.running.secureBoot);
    EnableWindow(GetDlgItem(window, IDC_BOOT_DISABLE), canChange);
    if (state.preview) SetDlgItemTextW(window, IDC_BOOT_NOTICE, L"Preview only. Boot changes are disabled.");
    else if (state.boot.running.secureBoot) SetDlgItemTextW(window, IDC_BOOT_NOTICE, L"Secure Boot protects Test Mode. Windows mode works without changing Secure Boot.");
    else SetDlgItemTextW(window, IDC_BOOT_NOTICE, L"A change takes effect after you restart Windows manually. Current session stays unchanged until then.");
    state.busy = false;
}

INT_PTR CALLBACK Procedure(HWND window, UINT message, WPARAM wp, LPARAM lp)
{
    auto state = reinterpret_cast<DialogState *>(GetWindowLongPtrW(window, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<DialogState *>(lp);
        SetWindowLongPtrW(window, DWLP_USER, lp);
        Refresh(window, *state);
        return TRUE;
    }
    if (!state) return FALSE;
    if (message == WM_CLOSE) { if (!state->busy) EndDialog(window, 0); return TRUE; }
    if (message != WM_COMMAND) return FALSE;
    if (state->busy) return TRUE;
    int id = LOWORD(wp);
    if (id == IDCANCEL) { EndDialog(window, 0); return TRUE; }
    if (id == IDC_BOOT_REFRESH) { Refresh(window, *state); return TRUE; }
    if (id != IDC_BOOT_ENABLE && id != IDC_BOOT_DISABLE) return FALSE;
    if (state->preview || !pn::IsElevated()) return TRUE;
    bool enable = id == IDC_BOOT_ENABLE;
    std::wstring question = enable
        ? L"Enable Windows Test Mode for the next start?\n\nThis allows test-signed kernel drivers to load and applies to this entire Windows installation. Restart Windows manually when you are ready."
        : L"Turn Test Mode OFF for the next start?\n\nTest-signed kernel drivers will no longer load. PN-A2DP devices must use Windows mode or a Microsoft-signed driver first. Restart Windows manually when you are ready.";
    state->busy = true;
    int answer = MessageBoxW(window, question.c_str(), L"PN-A2DP - Windows boot mode", MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2);
    if (answer == IDYES) {
        std::wstring detail;
        DWORD error = pn::ChangeBootMode(enable, detail);
        if (error && detail.empty()) detail = pn::Win32ErrorText(error);
        MessageBoxW(window, detail.c_str(), L"PN-A2DP - Windows boot mode", (error ? MB_ICONWARNING : MB_ICONINFORMATION) | MB_OK);
    }
    Refresh(window, *state);
    return TRUE;
}
}

int ShowBootModeDialog(HINSTANCE instance, HWND owner, bool preview)
{
    pn::BootDriverOperationLock operation;
    if (operation.error) {
        MessageBoxW(owner, L"Close the other boot-settings window or wait for the driver switch to finish, then try again.", L"PN-A2DP", MB_ICONINFORMATION);
        return (int)operation.error;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    DialogState state; state.preview = preview;
    INT_PTR result = DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_BOOT_MODE), owner, Procedure, (LPARAM)&state);
    return result == -1 ? (int)GetLastError() : (int)result;
}
