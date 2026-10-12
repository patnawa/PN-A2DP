/*
 * pndrv.h - driver package, device node, service and signing management for PN-A2DP.
 *
 * Wraps SetupAPI / newdev / SCM / crypt32 so that the control panel and the installer
 * can switch individual headsets between the inbox Microsoft A2DP driver and PN-A2DP,
 * stage or remove the driver package, manage the codec host service and inspect the
 * signing state of the machine. Functions that change the system need an elevated
 * process; see IsElevated() and RunElevated().
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <windows.h>
#include <string>
#include <vector>

namespace pn {

#define PN_INBOX_A2DP_INF        L"microsoft_bluetooth_a2dp_src.inf"
#define PN_DRIVER_SERVICE        L"pna2dp"
#define PN_HOST_SERVICE          L"pna2dp-codechost"
#define PN_HOST_DISPLAY_NAME     L"PN-A2DP Codec Host"
#define PN_HOST_DESCRIPTION      L"Encodes audio for the PN-A2DP Bluetooth driver (LDAC, aptX, AAC and other codec plugins)."
#define PN_INF_PROVIDER          L"PN-A2DP Project"
#define PN_REG_KEY               L"SOFTWARE\\PN-A2DP"
#define PN_ARP_KEY               L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PN-A2DP"

/* One BTHENUM device node for a remote AudioSink service (one per paired headset). */
struct A2dpNode {
    std::wstring instanceId;
    std::wstring name;           /* friendly name of the headset */
    std::wstring driverInf;      /* oem12.inf or microsoft_bluetooth_a2dp_src.inf */
    std::wstring driverProvider;
    std::wstring driverVersion;
    std::wstring driverDesc;
    std::wstring service;        /* pna2dp or BthA2dp */
    ULONGLONG    address = 0;    /* Bluetooth address parsed from the instance id */
    bool         present = false;   /* devnode exists (paired); false for a phantom node */
    bool         started = false;   /* its driver is running */
    bool         connected = false; /* Bluetooth link up (Windows' IsConnected property), else = started */
    bool         restartRequired = false; /* configured driver may differ from the running driver */
    ULONG        problemCode = 0;
    bool         usesPnA2dp() const { return _wcsicmp(service.c_str(), PN_DRIVER_SERVICE) == 0; }
    bool         usesWindowsA2dp() const { return _wcsicmp(service.c_str(), L"BthA2dp") == 0 || _wcsicmp(driverInf.c_str(), PN_INBOX_A2DP_INF) == 0; }
};

std::vector<A2dpNode> EnumerateA2dpSinkNodes(DWORD *error = nullptr);
bool GetA2dpNode(const std::wstring &instanceId, A2dpNode &node);

/* ---- driver package ---------------------------------------------------------- */
struct StagedPackage {
    std::wstring publishedName;  /* oem12.inf */
    std::wstring fullPath;       /* C:\Windows\INF\oem12.inf */
    std::wstring version;        /* DriverVer */
    std::wstring provider;
};
/* Numeric four-part DriverVer order, oldest first. OEM INF names are not versions. */
int CompareDriverVersions(const std::wstring &a, const std::wstring &b);
void SortStagedPackages(std::vector<StagedPackage> &packages);
std::vector<StagedPackage> FindStagedPackages();
DWORD StagePackage(const std::wstring &infPath, StagedPackage &out);          /* SetupCopyOEMInf */
DWORD UnstagePackage(const std::wstring &publishedName);                      /* SetupUninstallOEMInf */
DWORD InstallInfOnDevice(const std::wstring &instanceId, const std::wstring &infFullPath, bool &rebootRequired);
DWORD RestoreInboxDriver(const std::wstring &instanceId, bool &rebootRequired);
DWORD RescanDevices();
std::wstring InfDriverVersion(const std::wstring &infPath);                   /* "0.1.0.0" */

/* ---- signing / platform state ------------------------------------------------- */
struct SigningState {
    bool testSigning = false;
    bool secureBoot = false;
    bool hvci = false;           /* memory integrity actually running, not just configured */
    bool queried = false;
    bool secureBootKnown = false;
    bool hvciConfigured = false;
    bool hvciConfiguredKnown = false;
};
SigningState QuerySigningState();
std::wstring FileSignerName(const std::wstring &path);     /* subject CN of the embedded signature, empty if unsigned */
DWORD InstallCertificateToMachineStore(const std::wstring &cerPath, const wchar_t *storeName /* L"Root" / L"TrustedPublisher" */);
DWORD SetTestSigning(bool enable, std::wstring *detail = nullptr); /* current Windows entry only; reboot required */

/* ---- services ----------------------------------------------------------------- */
struct ServiceInfo {
    bool installed = false;
    DWORD state = 0;             /* SERVICE_RUNNING ... */
    DWORD startType = 0;
    std::wstring binaryPath;
};
bool  QueryService(const wchar_t *name, ServiceInfo &info);
DWORD StartServiceByName(const wchar_t *name, DWORD waitMs = 10000);
DWORD StopServiceByName(const wchar_t *name, DWORD waitMs = 10000);
DWORD StopServiceAndWaitForExit(const wchar_t *name, DWORD waitMs = 15000);   /* also waits until the service process is gone */
DWORD CreateHostService(const std::wstring &exePath);
DWORD DeleteServiceByName(const wchar_t *name);
const wchar_t *ServiceStateName(DWORD state);

/* ---- installation ------------------------------------------------------------- */
std::wstring InstallDir();                          /* registry InstallDir or %ProgramFiles%\PN-A2DP */
std::wstring ProgramFilesDir();
struct PluginInfo {
    std::wstring file;
    std::wstring name, version, license;
    ULONG codecMask = 0;
    bool valid = false;
};
std::vector<PluginInfo> EnumeratePlugins(const std::wstring &dir);

/* ---- process helpers ---------------------------------------------------------- */
bool  IsElevated();
DWORD RunElevated(const std::wstring &exe, const std::wstring &params, DWORD &exitCode, HWND owner = NULL);
DWORD RunAndWait(const std::wstring &cmdLine, DWORD &exitCode, std::wstring *output = nullptr);
std::wstring Win32ErrorText(DWORD err);
std::wstring ModulePath();
std::wstring ModuleDir();
bool FileExists(const std::wstring &path);
bool DirExists(const std::wstring &path);
DWORD CreateDirectoryRecursive(const std::wstring &path);
std::wstring JoinPath(const std::wstring &a, const std::wstring &b);
std::wstring BaseName(const std::wstring &path);
std::wstring ParseBtAddressFromInstanceId(const std::wstring &instanceId, ULONGLONG *addr);

} // namespace pn
