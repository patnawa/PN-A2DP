/* Run the production elevated-process wait without elevation or real operations.
 * A worker thread substitutes for the launched process, retaining real Windows
 * wait handles and synchronous cross-thread window messages. SPDX-License-Identifier: MIT */
#include "pndrv.h"
#include <shellapi.h>
#include <stdio.h>

namespace fixture {
constexpr UINT probeMessage = WM_APP + 19;
HWND window = nullptr;
DWORD mainThread = 0;
bool sendQuit = false, failExitQuery = false, invalidHandle = false;
DWORD launchError = ERROR_SUCCESS;
DWORD probeTimeout = 250;
int replies = 0;
bool enabledDuringWait = false;
LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    if (message == probeMessage) {
        ++replies;
        enabledDuringWait = IsWindowEnabled(hwnd) != FALSE;
        return 99;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
DWORD WINAPI Worker(void *) {
    if (sendQuit) PostThreadMessageW(mainThread, WM_QUIT, 42, 0);
    DWORD_PTR response = 0;
    return SendMessageTimeoutW(window, probeMessage, 0, 0, SMTO_BLOCK | SMTO_ABORTIFHUNG, probeTimeout, &response) && response == 99 ? 7 : ERROR_TIMEOUT;
}
BOOL WINAPI Launch(SHELLEXECUTEINFOW *info) {
    if (launchError) { SetLastError(launchError); return FALSE; }
    if (invalidHandle) {
        info->hProcess = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!info->hProcess) return FALSE;
        CloseHandle(info->hProcess); // a closed handle, not the -1 current-process pseudo-handle
        return TRUE;
    }
    info->hProcess = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return info->hProcess != nullptr;
}
BOOL WINAPI ExitCode(HANDLE handle, DWORD *code) {
    if (failExitQuery) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return GetExitCodeThread(handle, code);
}
void Reset() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {}
    sendQuit = failExitQuery = invalidHandle = false;
    launchError = ERROR_SUCCESS; probeTimeout = 250; replies = 0; enabledDuringWait = false;
    EnableWindow(window, TRUE);
}
}
#define ShellExecuteExW fixture::Launch
#define GetExitCodeProcess fixture::ExitCode
#include "../pndrv.cpp"

static int failures = 0, checks = 0;
#define CHECK(condition, text) do { ++checks; if (!(condition)) { ++failures; printf("FAIL: %s\n", text); } } while (0)
int main()
{
    fixture::mainThread = GetCurrentThreadId();
    WNDCLASSW cls{}; cls.lpfnWndProc = fixture::WindowProc; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = L"PnProcessWaitFixture";
    if (!RegisterClassW(&cls)) return 2;
    fixture::window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!fixture::window) return 2;
    DWORD exit = 0;
    CHECK(SendMessageW(fixture::window, fixture::probeMessage, 0, 0) == 99, "probe window and message handler work outside the wait");
    fixture::Reset();
    DWORD result = pn::RunElevated(L"fixture", L"", exit, fixture::window);
    CHECK(result == ERROR_SUCCESS && exit == 7 && fixture::replies == 1, "UI answers helper messages before the helper timeout");
    CHECK(fixture::replies == 1 && !fixture::enabledDuringWait && IsWindowEnabled(fixture::window), "owner input disabled during operation and restored after completion");
    printf("  message loop: result=%lu helperExit=%lu replies=%d\n", result, exit, fixture::replies);
    fixture::Reset(); fixture::probeTimeout = 1000;
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_SUCCESS && exit == 7, "longer helper deadline still receives an immediate reply");
    fixture::Reset(); fixture::sendQuit = true;
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_SUCCESS && exit == 7, "quit does not abandon an active helper");
    MSG quit{};
    CHECK(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 42, "quit code preserved for caller message loop");
    fixture::Reset(); fixture::failExitQuery = true;
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_ACCESS_DENIED, "exit-code query failure propagated");
    CHECK(IsWindowEnabled(fixture::window), "owner restored after exit-code failure");
    fixture::Reset(); fixture::invalidHandle = true;
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_INVALID_HANDLE, "invalid wait handle is an error, not success");
    CHECK(IsWindowEnabled(fixture::window), "owner restored after wait failure");
    fixture::Reset(); fixture::launchError = ERROR_CANCELLED;
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_CANCELLED && IsWindowEnabled(fixture::window), "cancelled launch preserves owner state");
    fixture::Reset(); EnableWindow(fixture::window, FALSE);
    CHECK(pn::RunElevated(L"fixture", L"", exit, fixture::window) == ERROR_SUCCESS && !IsWindowEnabled(fixture::window), "previously disabled owner stays disabled");
    fixture::Reset();
    CHECK(pn::RunElevated(L"fixture", L"", exit) == ERROR_SUCCESS && exit == 7, "ownerless caller still dispatches its Windows messages");
    DestroyWindow(fixture::window); UnregisterClassW(cls.lpszClassName, cls.hInstance);
    printf("Process wait: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
