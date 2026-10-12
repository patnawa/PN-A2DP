/*
 * main.cpp - PN-A2DP codec host: runs as a Windows service or as a console program.
 *
 *   pna2dp-codechost.exe                 console mode (Ctrl+C to stop)
 *   pna2dp-codechost.exe --install       register the service (run elevated)
 *   pna2dp-codechost.exe --uninstall
 *   pna2dp-codechost.exe --service       entry used by the Service Control Manager
 *   pna2dp-codechost.exe --list-plugins  inspect discovery without opening devices
 *
 * The host watches for PN-A2DP device interfaces (one per paired headset served by the
 * driver), opens each, registers the codecs its plugins provide and encodes on demand.
 *
 * SPDX-License-Identifier: MIT
 */
#include "host.h"
#include "../../tools/common/pnsecurepath.h"
#include "../../common/recovery_policy.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <stdio.h>
#include <map>

#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")

static const wchar_t *kServiceName = L"pna2dp-codechost";
static SERVICE_STATUS_HANDLE g_statusHandle = NULL;
static SERVICE_STATUS g_status = {};
static HANDLE g_stopEvent = NULL;

static bool EnumerateDevices(std::vector<std::wstring> &out)
{
    out.clear();
    ULONG len = 0;
    if (CM_Get_Device_Interface_List_SizeW(&len, (LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
    if (len < 2) return true;
    std::vector<WCHAR> buf(len);
    if (CM_Get_Device_Interface_ListW((LPGUID)&GUID_DEVINTERFACE_PNA2DP, NULL, buf.data(), len, CM_GET_DEVICE_INTERFACE_LIST_PRESENT) != CR_SUCCESS) return false;
    for (PWCHAR p = buf.data(); *p; p += wcslen(p) + 1) out.push_back(p);
    return true;
}

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    DWORD length = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return {};
    std::wstring s(path);
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? L"" : s.substr(0, p);
}

static void DiscoverPlugins(PluginRegistry &plugins)
{
    plugins.AddBuiltin(PnBuiltinSbcPlugin());
    auto directory = ExeDir();
    if (!directory.empty()) {
        plugins.LoadDirectory(directory);
        plugins.LoadDirectory(directory + L"\\plugins");
    }
    // This host runs as LocalSystem. ProgramData children can be created by an
    // ordinary user, so they must never be searched for executable plugins.
    HostLog("codec mask 0x%08x", plugins.CodecMask());
}

static void RunHost(HANDLE stopEvent)
{
    PluginRegistry plugins;
    DiscoverPlugins(plugins);

    std::map<std::wstring, std::unique_ptr<DeviceWorker>> workers;
    std::map<std::wstring, PnHostRetry> retries;
    /* Poll for interface arrival/removal; CM_Register_Notification would be the fancier way. */
    while (WaitForSingleObject(stopEvent, 2000) == WAIT_TIMEOUT) {
        std::vector<std::wstring> devs;
        if (!EnumerateDevices(devs)) continue; // query failure is not device removal
        ULONGLONG now = GetTickCount64();
        for (auto &d : devs) {
            auto it = workers.find(d);
            auto &retry = retries[d];
            if (it == workers.end() || !it->second->Alive()) {
                // Release the old handle's exclusive codec registration before replacing it.
                if (it != workers.end()) { it->second->Stop(); workers.erase(it); PnHostRetryFailed(&retry, now); }
                if (now < retry.next) continue;
                auto w = std::make_unique<DeviceWorker>(d, plugins);
                if (w->Start()) { workers[d] = std::move(w); retry.started = now; }
                else PnHostRetryFailed(&retry, now);
            } else PnHostRetryHealthy(&retry, now);
        }
        for (auto it = retries.begin(); it != retries.end();) {
            bool present = false; for (auto &d : devs) if (d == it->first) present = true;
            if (!present) it = retries.erase(it); else ++it;
        }
        for (auto it = workers.begin(); it != workers.end();) {
            bool present = false;
            for (auto &d : devs) if (d == it->first) present = true;
            if (!present) { it->second->Stop(); it = workers.erase(it); } else ++it;
        }
    }
    for (auto &w : workers) w.second->Stop();
    workers.clear();
}

/* ---- service plumbing ------------------------------------------------------ */

static VOID WINAPI ServiceCtrlHandler(DWORD ctrl)
{
    if (ctrl == SERVICE_CONTROL_STOP || ctrl == SERVICE_CONTROL_SHUTDOWN) {
        g_status.dwCurrentState = SERVICE_STOP_PENDING;
        SetServiceStatus(g_statusHandle, &g_status);
        SetEvent(g_stopEvent);
    }
}

static VOID WINAPI ServiceMain(DWORD, LPWSTR *)
{
    g_statusHandle = RegisterServiceCtrlHandlerW(kServiceName, ServiceCtrlHandler);
    if (!g_statusHandle) return;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!g_stopEvent) {
        g_status.dwCurrentState = SERVICE_STOPPED;
        g_status.dwWin32ExitCode = GetLastError();
        SetServiceStatus(g_statusHandle, &g_status);
        return;
    }
    g_status.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    g_status.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(g_statusHandle, &g_status);
    RunHost(g_stopEvent);
    g_status.dwCurrentState = SERVICE_STOPPED;
    g_status.dwControlsAccepted = 0;
    SetServiceStatus(g_statusHandle, &g_status);
    CloseHandle(g_stopEvent);
    g_stopEvent = NULL;
}

static int InstallService(bool install)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) { fprintf(stderr, "OpenSCManager failed %lu (run elevated)\n", GetLastError()); return 1; }
    if (install) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(NULL, path, MAX_PATH);
        DWORD safety = pn::ValidateProtectedService(path);
        if (safety) { fprintf(stderr, "Unsafe service location (%lu). Install in an administrator-protected folder.\n", safety); CloseServiceHandle(scm); return (int)safety; }
        std::wstring cmd = std::wstring(L"\"") + path + L"\" --service";
        SC_HANDLE s = CreateServiceW(scm, kServiceName, L"PN-A2DP Codec Host", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                     SERVICE_ERROR_NORMAL, cmd.c_str(), NULL, NULL, NULL, NULL, NULL);
        if (!s) { fprintf(stderr, "CreateService failed %lu\n", GetLastError()); CloseServiceHandle(scm); return 1; }
        SERVICE_DESCRIPTIONW d = { (LPWSTR)L"Encodes audio for the PN-A2DP Bluetooth driver (LDAC, aptX, AAC and other codec plugins)." };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        StartServiceW(s, 0, NULL);
        CloseServiceHandle(s);
        printf("service installed and started\n");
    } else {
        SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (!s) { fprintf(stderr, "service not found\n"); CloseServiceHandle(scm); return 1; }
        SERVICE_STATUS st;
        ControlService(s, SERVICE_CONTROL_STOP, &st);
        DeleteService(s);
        CloseServiceHandle(s);
        printf("service removed\n");
    }
    CloseServiceHandle(scm);
    return 0;
}

static BOOL WINAPI ConsoleCtrl(DWORD) { SetEvent(g_stopEvent); return TRUE; }

int wmain(int argc, wchar_t **argv)
{
    // Uses the service's discovery path without opening any audio device.
    if (argc > 1 && !wcscmp(argv[1], L"--list-plugins")) {
        PluginRegistry plugins;
        DiscoverPlugins(plugins);
        return 0;
    }
    if (argc > 1 && !wcscmp(argv[1], L"--install")) return InstallService(true);
    if (argc > 1 && !wcscmp(argv[1], L"--uninstall")) return InstallService(false);
    if (argc > 1 && !wcscmp(argv[1], L"--service")) {
        wchar_t path[MAX_PATH]{};
        DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        DWORD safety = length && length < MAX_PATH ? pn::ValidateProtectedService(path) : ERROR_ACCESS_DENIED;
        if (safety) { HostLog("Refusing unsafe service/plugin location (%lu)", safety); return (int)safety; }
        SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)kServiceName, ServiceMain }, { NULL, NULL } };
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    g_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
    HostLog("PN-A2DP codec host (console mode)");
    RunHost(g_stopEvent);
    return 0;
}
