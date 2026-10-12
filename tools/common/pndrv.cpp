/*
 * pndrv.cpp - driver package, device node, service and signing management for PN-A2DP.
 * SPDX-License-Identifier: MIT
 */
#include "pndrv.h"
#include "pnsecurepath.h"
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincrypt.h>
#include <winternl.h>
#include <stdio.h>
#include <algorithm>
#include <cwctype>
#include "pna2dp_public.h"
#include "pna2dp_codec_plugin.h"

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ole32.lib")

namespace pn {

/* ------------------------------------------------------------------------- */
/* Small helpers                                                                */
/* ------------------------------------------------------------------------- */

/* SetupAPI's own error codes (0xE0000xxx) have no system message; these are the ones the
 * driver tooling can run into. */
static const wchar_t *SetupApiErrorText(DWORD err)
{
    switch (err) {
    case 0xE0000203: return L"The INF section is missing";
    case 0xE000020B: return L"The device instance does not exist";
    case 0xE0000217: return L"The INF has no matching device";
    case 0xE000022F: return L"The driver package has no catalog (it is not signed); Windows refuses unsigned kernel drivers unless test signing is enabled";
    case 0xE0000230: return L"The driver store rejected the package";
    case 0xE0000231: return L"The INF name is already in use (DI_FLAGSEX_SETFAILEDINSTALL)";
    case 0xE0000234: return L"The device has no compatible drivers in the selected INF";
    case 0xE0000235: return L"The driver is not a compatible driver for this device";
    case 0xE0000236: return L"The driver installation is blocked by the device's PnP state";
    case 0xE0000237: return L"The INF file is not in the driver store";
    case 0xE0000238: return L"No class installer is available";
    case 0xE0000239: return L"The device installer is not ready";
    case 0xE000023E: return L"The INF does not contain a valid [Version] section";
    case 0xE0000240: return L"The driver package is not in the driver store";
    case 0xE0000242: return L"The driver store add operation failed";
    case 0xE0000243: return L"The catalog data in the driver package is invalid";
    case 0xE0000244: return L"The driver signature does not match the catalog (the .sys was rebuilt after signing, or the catalog is for another OS)";
    case 0xE0000245: return L"The driver package was signed but the certificate is not trusted on this machine";
    case 0xE0000246: return L"Driver installation was blocked by policy";
    case 0xE0000247: return L"The third-party INF does not contain digital signature information";
    case 0xE000024B: return L"The driver package is not supported on this platform";
    case 0xE0000300: return L"The INF has no [DefaultInstall] section";
    case 0xE0000302: return L"The driver package install is blocked by the running Windows edition or Secure Boot policy";
    case 0xE0000304: return L"The device cannot be installed through this API because it is already in a transaction";
    default: return nullptr;
    }
}

std::wstring Win32ErrorText(DWORD err)
{
    wchar_t msg[512] = {};
    DWORD n = 0;
    const wchar_t *fixed = SetupApiErrorText(err);
    if (fixed) { wcscpy_s(msg, fixed); n = (DWORD)wcslen(msg); }
    else n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS | FORMAT_MESSAGE_MAX_WIDTH_MASK, NULL, err, 0, msg, 511, NULL);
    while (n && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n' || msg[n - 1] == L' ')) msg[--n] = 0;
    wchar_t out[600];
    if (n) swprintf_s(out, L"%s (error %lu, 0x%08lx)", msg, err, err);
    else swprintf_s(out, L"error %lu (0x%08lx)", err, err);
    return out;
}

std::wstring ModulePath()
{
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(NULL, p, MAX_PATH);
    return p;
}

std::wstring ModuleDir()
{
    std::wstring p = ModulePath();
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

bool FileExists(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring &path)
{
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

DWORD CreateDirectoryRecursive(const std::wstring &path)
{
    if (DirExists(path)) return ERROR_SUCCESS;
    size_t s = path.find_last_of(L"\\/");
    if (s != std::wstring::npos && s > 2) {
        DWORD r = CreateDirectoryRecursive(path.substr(0, s));
        if (r != ERROR_SUCCESS) return r;
    }
    if (!CreateDirectoryW(path.c_str(), NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return GetLastError();
    return ERROR_SUCCESS;
}

std::wstring JoinPath(const std::wstring &a, const std::wstring &b)
{
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring BaseName(const std::wstring &path)
{
    size_t s = path.find_last_of(L"\\/");
    return s == std::wstring::npos ? path : path.substr(s + 1);
}

std::wstring ParseBtAddressFromInstanceId(const std::wstring &id, ULONGLONG *addr)
{
    /* BTHENUM\{0000110b-...}_LOCALMFG&000f\7&1a2b3c4d&0&E8EC4A123456_C00000000 */
    if (addr) *addr = 0;
    size_t s = id.find_last_of(L'\\');
    std::wstring tail = s == std::wstring::npos ? id : id.substr(s + 1);
    size_t amp = tail.find_last_of(L'&');
    if (amp == std::wstring::npos) return L"";
    std::wstring hex = tail.substr(amp + 1, 12);
    if (hex.size() != 12) return L"";
    ULONGLONG v = 0;
    for (wchar_t c : hex) {
        v <<= 4;
        if (c >= L'0' && c <= L'9') v |= c - L'0';
        else if (c >= L'a' && c <= L'f') v |= c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v |= c - L'A' + 10;
        else return L"";
    }
    if (addr) *addr = v;
    return hex;
}

/* ------------------------------------------------------------------------- */
/* Device nodes                                                                 */
/* ------------------------------------------------------------------------- */

static std::wstring GetStringProperty(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key)
{
    DEVPROPTYPE type = 0;
    wchar_t buf[1024] = {};
    DWORD size = 0;
    if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)buf, sizeof(buf) - 2, &size, 0)) return L"";
    if (type != DEVPROP_TYPE_STRING) return L"";
    return buf;
}

static ULONG GetUlongProperty(HDEVINFO set, SP_DEVINFO_DATA &dev, const DEVPROPKEY &key, ULONG def = 0)
{
    DEVPROPTYPE type = 0;
    ULONG v = def;
    DWORD size = 0;
    if (!SetupDiGetDevicePropertyW(set, &dev, &key, &type, (PBYTE)&v, sizeof(v), &size, 0)) return def;
    return (type == DEVPROP_TYPE_UINT32 || type == DEVPROP_TYPE_INT32) ? v : def;
}

static std::wstring BluetoothDeviceName(ULONGLONG address)
{
    if (!address) return {};
    // The inbox A2DP service is named "Microsoft Bluetooth A2dp Source".
    // Its paired Bluetooth device is a sibling PDO; match the actual address,
    // never infer identity by the audio endpoint's display name.
    wchar_t prefix[64]; swprintf_s(prefix, L"BTHENUM\\DEV_%012llX\\", address);
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return {};
    std::wstring name;
    SP_DEVINFO_DATA dev = {sizeof(dev)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &dev); ++i) {
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, _countof(id), nullptr) || _wcsnicmp(id, prefix, wcslen(prefix))) continue;
        auto candidate = GetStringProperty(set, dev, DEVPKEY_Device_FriendlyName);
        if (candidate.empty()) candidate = GetStringProperty(set, dev, DEVPKEY_NAME);
        if (!candidate.empty()) { name = candidate; break; }
    }
    SetupDiDestroyDeviceInfoList(set);
    return name;
}

static void FillNode(HDEVINFO set, SP_DEVINFO_DATA &dev, const std::wstring &id, A2dpNode &n)
{
    n.instanceId = id;
    n.name = GetStringProperty(set, dev, DEVPKEY_Device_FriendlyName);
    if (n.name.empty()) n.name = GetStringProperty(set, dev, DEVPKEY_NAME);
    if (n.name.empty()) n.name = GetStringProperty(set, dev, DEVPKEY_Device_DeviceDesc);
    n.driverInf = GetStringProperty(set, dev, DEVPKEY_Device_DriverInfPath);
    n.driverProvider = GetStringProperty(set, dev, DEVPKEY_Device_DriverProvider);
    n.driverVersion = GetStringProperty(set, dev, DEVPKEY_Device_DriverVersion);
    n.driverDesc = GetStringProperty(set, dev, DEVPKEY_Device_DriverDesc);
    n.service = GetStringProperty(set, dev, DEVPKEY_Device_Service);
    /* present = the devnode exists right now (a phantom node of a paired but absent headset
     * fails CM_Get_DevNode_Status); started = its driver is running. */
    ULONG st = 0, pn2 = 0;
    n.present = CM_Get_DevNode_Status(&st, &pn2, dev.DevInst, 0) == CR_SUCCESS;
    n.started = n.present && (st & DN_STARTED) != 0;
    SP_DEVINSTALL_PARAMS_W install = {sizeof(install)};
    n.restartRequired = (n.present && (st & DN_NEED_RESTART) != 0) ||
        (SetupDiGetDeviceInstallParamsW(set, &dev, &install) && (install.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)) != 0);
    n.problemCode = n.present ? pn2 : GetUlongProperty(set, dev, DEVPKEY_Device_ProblemCode);
    /* Bluetooth connection state lives on the device's own node (the parent of the service PDO) */
    static const DEVPROPKEY kIsConnected = { { 0x83da6326, 0x97a6, 0x4088, { 0x94, 0x53, 0xa1, 0x92, 0x3f, 0x57, 0x3b, 0x29 } }, 15 };
    n.connected = n.started;
    DEVPROPTYPE type = 0;
    DEVPROP_BOOLEAN b = DEVPROP_FALSE;
    DWORD size = 0;
    if (SetupDiGetDevicePropertyW(set, &dev, &kIsConnected, &type, (PBYTE)&b, sizeof(b), &size, 0) && type == DEVPROP_TYPE_BOOLEAN) {
        n.connected = b != DEVPROP_FALSE;
    } else {
        std::wstring parent = GetStringProperty(set, dev, DEVPKEY_Device_Parent);
        if (!parent.empty()) {
            HDEVINFO pset = SetupDiCreateDeviceInfoList(NULL, NULL);
            if (pset != INVALID_HANDLE_VALUE) {
                SP_DEVINFO_DATA pdev = { sizeof(pdev) };
                if (SetupDiOpenDeviceInfoW(pset, parent.c_str(), NULL, 0, &pdev) &&
                    SetupDiGetDevicePropertyW(pset, &pdev, &kIsConnected, &type, (PBYTE)&b, sizeof(b), &size, 0) && type == DEVPROP_TYPE_BOOLEAN)
                    n.connected = b != DEVPROP_FALSE;
                SetupDiDestroyDeviceInfoList(pset);
            }
        }
    }
    ParseBtAddressFromInstanceId(id, &n.address);
    auto bluetoothName = BluetoothDeviceName(n.address);
    if (!bluetoothName.empty()) n.name = bluetoothName;
}

static bool IsA2dpSinkInstance(const std::wstring &id)
{
    return _wcsnicmp(id.c_str(), L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}", 45) == 0;
}

std::vector<A2dpNode> EnumerateA2dpSinkNodes(DWORD *error)
{
    if (error) *error = ERROR_SUCCESS;
    std::vector<A2dpNode> out;
    HDEVINFO set = SetupDiGetClassDevsW(NULL, L"BTHENUM", NULL, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) { if (error) *error = GetLastError(); return out; }
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    for (DWORD i = 0;; i++) {
        if (!SetupDiEnumDeviceInfo(set, i, &dev)) {
            DWORD result = GetLastError();
            if (error && result != ERROR_NO_MORE_ITEMS) *error = result;
            break;
        }
        wchar_t id[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(set, &dev, id, MAX_DEVICE_ID_LEN, NULL)) { if (error) *error = GetLastError(); continue; }
        if (!IsA2dpSinkInstance(id)) continue;
        A2dpNode n;
        FillNode(set, dev, id, n);
        out.push_back(n);
    }
    SetupDiDestroyDeviceInfoList(set);
    return out;
}

bool GetA2dpNode(const std::wstring &instanceId, A2dpNode &node)
{
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return false;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    bool ok = SetupDiOpenDeviceInfoW(set, instanceId.c_str(), NULL, 0, &dev) != FALSE;
    if (ok) FillNode(set, dev, instanceId, node);
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

/* ------------------------------------------------------------------------- */
/* Driver package                                                               */
/* ------------------------------------------------------------------------- */

static std::wstring InfValue(HINF inf, const wchar_t *section, const wchar_t *key)
{
    wchar_t buf[512] = {};
    DWORD req = 0;
    if (!SetupGetLineTextW(NULL, inf, section, key, buf, 511, &req)) return L"";
    return buf;
}

std::wstring InfDriverVersion(const std::wstring &infPath)
{
    HINF h = SetupOpenInfFileW(infPath.c_str(), NULL, INF_STYLE_WIN4, NULL);
    if (h == INVALID_HANDLE_VALUE) return L"";
    std::wstring v = InfValue(h, L"Version", L"DriverVer");    /* "10/10/2026,0.1.0.0" */
    SetupCloseInfFile(h);
    size_t c = v.find(L',');
    return c == std::wstring::npos ? v : v.substr(c + 1);
}

static bool IsOurInf(const std::wstring &path, std::wstring *version, std::wstring *provider)
{
    HINF h = SetupOpenInfFileW(path.c_str(), NULL, INF_STYLE_WIN4, NULL);
    if (h == INVALID_HANDLE_VALUE) return false;
    INFCONTEXT ctx;
    bool ours = SetupFindFirstLineW(h, L"SourceDisksFiles", L"pna2dp.sys", &ctx) != FALSE;
    if (ours) {
        std::wstring v = InfValue(h, L"Version", L"DriverVer");
        size_t c = v.find(L',');
        if (version) *version = c == std::wstring::npos ? v : v.substr(c + 1);
        if (provider) *provider = InfValue(h, L"Version", L"Provider");
    }
    SetupCloseInfFile(h);
    return ours;
}

static bool ParseDriverVersion(const std::wstring &text, ULONGLONG &version)
{
    const wchar_t *p = text.c_str();
    while (iswspace(*p)) ++p;
    version = 0;
    for (unsigned part = 0; part < 4; ++part) {
        if (*p < L'0' || *p > L'9') return false;
        unsigned value = 0;
        while (*p >= L'0' && *p <= L'9') {
            value = value * 10 + (*p++ - L'0');
            if (value > 65535) return false;
        }
        version = (version << 16) | value;
        if (part < 3 && *p++ != L'.') return false;
    }
    while (iswspace(*p)) ++p;
    return *p == 0;
}

int CompareDriverVersions(const std::wstring &a, const std::wstring &b)
{
    ULONGLONG av = 0, bv = 0;
    bool aValid = ParseDriverVersion(a, av), bValid = ParseDriverVersion(b, bv);
    if (aValid != bValid) return aValid ? 1 : -1;
    if (!aValid) return 0;
    return av < bv ? -1 : av > bv ? 1 : 0;
}

void SortStagedPackages(std::vector<StagedPackage> &packages)
{
    std::sort(packages.begin(), packages.end(), [](const StagedPackage &a, const StagedPackage &b) {
        int version = CompareDriverVersions(a.version, b.version);
        if (version) return version < 0;
        // Stable choice for identical versions, independent of filesystem enumeration.
        return _wcsicmp(a.fullPath.c_str(), b.fullPath.c_str()) < 0;
    });
}

std::vector<StagedPackage> FindStagedPackages()
{
    std::vector<StagedPackage> out;
    wchar_t windir[MAX_PATH];
    GetWindowsDirectoryW(windir, MAX_PATH);
    std::wstring dir = JoinPath(windir, L"INF");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(JoinPath(dir, L"oem*.inf").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        StagedPackage p;
        p.publishedName = fd.cFileName;
        p.fullPath = JoinPath(dir, fd.cFileName);
        if (IsOurInf(p.fullPath, &p.version, &p.provider)) out.push_back(p);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    SortStagedPackages(out);
    return out;
}

static DWORD ComparePackageFile(const std::wstring &source, const std::wstring &stored)
{
    HANDLE a = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (a == INVALID_HANDLE_VALUE) return GetLastError();
    HANDLE b = CreateFileW(stored.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (b == INVALID_HANDLE_VALUE) { DWORD err = GetLastError(); CloseHandle(a); return err; }
    DWORD err = ERROR_SUCCESS;
    BYTE ba[4096], bb[4096];
    for (;;) {
        DWORD na = 0, nb = 0;
        if (!ReadFile(a, ba, sizeof(ba), &na, NULL) || !ReadFile(b, bb, sizeof(bb), &nb, NULL)) { err = GetLastError(); break; }
        if (na != nb || memcmp(ba, bb, na)) { err = ERROR_REVISION_MISMATCH; break; }
        if (!na) break;
    }
    CloseHandle(b);
    CloseHandle(a);
    return err;
}

DWORD StagePackage(const std::wstring &infPath, StagedPackage &out)
{
    wchar_t dest[MAX_PATH] = {};
    PWSTR comp = NULL;
    if (!SetupCopyOEMInfW(infPath.c_str(), NULL, SPOST_PATH, 0, dest, MAX_PATH, NULL, &comp)) return GetLastError();
    // SetupAPI can reuse a published INF. Do not claim success for an older
    // binary when a development build reused the package's version/identity.
    wchar_t storedInf[MAX_PATH] = {};
    if (!SetupGetInfDriverStoreLocationW(dest, NULL, NULL, storedInf, MAX_PATH, NULL)) return GetLastError();
    std::wstring sourceDir = infPath.substr(0, infPath.find_last_of(L"\\/"));
    std::wstring storeDir = storedInf;
    storeDir.resize(storeDir.find_last_of(L"\\/"));
    for (const wchar_t *file : { L"pna2dp.sys", L"pna2dp.cat" }) {
        DWORD err = ComparePackageFile(JoinPath(sourceDir, file), JoinPath(storeDir, file));
        if (err != ERROR_SUCCESS) return err;
    }
    out.fullPath = dest;
    out.publishedName = comp ? comp : BaseName(dest);
    IsOurInf(out.fullPath, &out.version, &out.provider);
    return ERROR_SUCCESS;
}

DWORD UnstagePackage(const std::wstring &publishedName)
{
    if (!SetupUninstallOEMInfW(publishedName.c_str(), SUOI_FORCEDELETE, NULL)) return GetLastError();
    return ERROR_SUCCESS;
}

/* Select a specific INF for a device node and install it, the way Device Manager's
 * "Have Disk" update does. Driver ranking would otherwise keep the inbox driver. */
DWORD InstallInfOnDevice(const std::wstring &instanceId, const std::wstring &infFullPath, bool &reboot)
{
    reboot = false;
    /* An explicit install is a fresh start for the driver's crash guard (see regcfg.h);
     * a count left from an earlier build would keep it from loading (Code 37). */
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\pna2dp\\State", L"UncleanStarts");
    RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\pna2dp\\Parameters", L"UncleanStarts");
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = ERROR_SUCCESS;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    SP_DEVINSTALL_PARAMS_W p = { sizeof(p) };
    SP_DRVINFO_DATA_W drv = { sizeof(drv) };
    BOOL rb = FALSE;
    if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), NULL, 0, &dev)) { err = GetLastError(); goto done; }
    if (!SetupDiGetDeviceInstallParamsW(set, &dev, &p)) { err = GetLastError(); goto done; }
    p.Flags |= DI_ENUMSINGLEINF;
    p.FlagsEx |= DI_FLAGSEX_ALLOWEXCLUDEDDRVS;
    wcscpy_s(p.DriverPath, infFullPath.c_str());
    if (!SetupDiSetDeviceInstallParamsW(set, &dev, &p)) { err = GetLastError(); goto done; }
    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER)) { err = GetLastError(); goto done; }
    if (!SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, 0, &drv)) { err = GetLastError(); if (err == ERROR_NO_MORE_ITEMS) err = ERROR_NO_COMPAT_DRIVERS; goto done; }
    if (!SetupDiSetSelectedDriverW(set, &dev, &drv)) { err = GetLastError(); goto done; }
    // Supplying &drv selects this driver explicitly. DiInstallDevice's 0x2 flag
    // suppresses finish-install UI; DIIRFLAG_FORCE_INF belongs to DiInstallDriver.
    if (!DiInstallDevice(NULL, set, &dev, &drv, DIIDFLAG_NOFINISHINSTALLUI, &rb)) { err = GetLastError(); goto done; }
    reboot = rb != FALSE;
done:
    SetupDiDestroyDeviceInfoList(set);
    return err;
}

/* Put the Microsoft inbox A2DP driver back on one node: enumerate the compatible drivers,
 * pick the inbox INF and install it (no forcing needed; it ranks first anyway). */
DWORD RestoreInboxDriver(const std::wstring &instanceId, bool &reboot)
{
    reboot = false;
    HDEVINFO set = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (set == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = ERROR_SUCCESS;
    SP_DEVINFO_DATA dev = { sizeof(dev) };
    SP_DRVINFO_DATA_W drv = { sizeof(drv) }, chosen = { sizeof(chosen) }, fallback = { sizeof(fallback) };
    bool found = false, haveFallback = false;
    BOOL rb = FALSE;
    if (!SetupDiOpenDeviceInfoW(set, instanceId.c_str(), NULL, 0, &dev)) { err = GetLastError(); goto done; }
    if (!SetupDiBuildDriverInfoList(set, &dev, SPDIT_COMPATDRIVER)) { err = GetLastError(); goto done; }
    for (DWORD i = 0; SetupDiEnumDriverInfoW(set, &dev, SPDIT_COMPATDRIVER, i, &drv); i++) {
        BYTE buf[sizeof(SP_DRVINFO_DETAIL_DATA_W) + 2048] = {};
        SP_DRVINFO_DETAIL_DATA_W *det = (SP_DRVINFO_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof(SP_DRVINFO_DETAIL_DATA_W);
        SetupDiGetDriverInfoDetailW(set, &dev, &drv, det, sizeof(buf), NULL);
        std::wstring inf = BaseName(det->InfFileName);
        if (_wcsicmp(inf.c_str(), PN_INBOX_A2DP_INF) == 0) { chosen = drv; found = true; break; }
        /* fallback: a Microsoft inbox A2DP driver under another INF name; never bth.inf's generic
         * "Audio Sink Service" entry, which precedes the real driver in the compatible list */
        bool a2dp = wcsstr(drv.Description, L"A2dp") || wcsstr(drv.Description, L"A2DP") || wcsstr(det->SectionName, L"A2dp");
        bool inbox = _wcsicmp(drv.ProviderName, L"Microsoft") == 0 && _wcsnicmp(inf.c_str(), L"oem", 3) != 0;
        if (!haveFallback && inbox && a2dp) { fallback = drv; haveFallback = true; }
    }
    if (!found && haveFallback) { chosen = fallback; found = true; }
    if (!found) { err = ERROR_NOT_FOUND; goto done; }
    if (!SetupDiSetSelectedDriverW(set, &dev, &chosen)) { err = GetLastError(); goto done; }
    if (!DiInstallDevice(NULL, set, &dev, &chosen, 0, &rb)) { err = GetLastError(); goto done; }
    reboot = rb != FALSE;
done:
    SetupDiDestroyDeviceInfoList(set);
    return err;
}

DWORD RescanDevices()
{
    DEVINST root = 0;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return ERROR_NOT_FOUND;
    return CM_Reenumerate_DevNode(root, CM_REENUMERATE_NORMAL) == CR_SUCCESS ? ERROR_SUCCESS : ERROR_GEN_FAILURE;
}

/* ------------------------------------------------------------------------- */
/* Signing state                                                                */
/* ------------------------------------------------------------------------- */

typedef struct _PN_SYSTEM_CODEINTEGRITY_INFORMATION { ULONG Length; ULONG CodeIntegrityOptions; } PN_SYSTEM_CODEINTEGRITY_INFORMATION;
#define PN_SystemCodeIntegrityInformation 103
#define PN_CODEINTEGRITY_OPTION_TESTSIGN 0x02

SigningState QuerySigningState()
{
    SigningState s;
    typedef NTSTATUS(NTAPI *QSI)(ULONG, PVOID, ULONG, PULONG);
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    QSI q = nt ? (QSI)GetProcAddress(nt, "NtQuerySystemInformation") : NULL;
    if (q) {
        PN_SYSTEM_CODEINTEGRITY_INFORMATION ci = { sizeof(ci), 0 };
        ULONG ret = 0;
        if (q(PN_SystemCodeIntegrityInformation, &ci, sizeof(ci), &ret) >= 0) {
            s.testSigning = (ci.CodeIntegrityOptions & PN_CODEINTEGRITY_OPTION_TESTSIGN) != 0;
            s.hvci = (ci.CodeIntegrityOptions & 0x400) != 0; // HVCI_KMCI_ENABLED (running)
            s.queried = true;
        }
    }
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State", L"UEFISecureBootEnabled", RRF_RT_REG_DWORD, NULL, &v, &size) == ERROR_SUCCESS) {
        s.secureBoot = v != 0; s.secureBootKnown = true;
    } else {
        FIRMWARE_TYPE firmware = FirmwareTypeUnknown;
        if (GetFirmwareType(&firmware) && firmware == FirmwareTypeBios) s.secureBootKnown = true;
    }
    v = 0; size = sizeof(v);
    LSTATUS status = RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\DeviceGuard\\Scenarios\\HypervisorEnforcedCodeIntegrity", L"Enabled", RRF_RT_REG_DWORD, NULL, &v, &size);
    s.hvciConfiguredKnown = status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND;
    s.hvciConfigured = status == ERROR_SUCCESS && v != 0;
    return s;
}

std::wstring FileSignerName(const std::wstring &path)
{
    HCERTSTORE store = NULL;
    HCRYPTMSG msg = NULL;
    DWORD enc = 0, content = 0, format = 0;
    std::wstring result;
    if (!CryptQueryObject(CERT_QUERY_OBJECT_FILE, path.c_str(), CERT_QUERY_CONTENT_FLAG_PKCS7_SIGNED_EMBED, CERT_QUERY_FORMAT_FLAG_BINARY, 0, &enc, &content, &format, &store, &msg, NULL))
        return L"";
    DWORD size = 0;
    if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, NULL, &size) && size) {
        std::vector<BYTE> buf(size);
        CMSG_SIGNER_INFO *si = (CMSG_SIGNER_INFO *)buf.data();
        if (CryptMsgGetParam(msg, CMSG_SIGNER_INFO_PARAM, 0, si, &size)) {
            CERT_INFO ci = {};
            ci.Issuer = si->Issuer;
            ci.SerialNumber = si->SerialNumber;
            PCCERT_CONTEXT cert = CertFindCertificateInStore(store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0, CERT_FIND_SUBJECT_CERT, &ci, NULL);
            if (cert) {
                wchar_t name[256] = {};
                CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256);
                result = name;
                CertFreeCertificateContext(cert);
            }
        }
    }
    if (msg) CryptMsgClose(msg);
    if (store) CertCloseStore(store, 0);
    return result;
}

DWORD InstallCertificateToMachineStore(const std::wstring &cerPath, const wchar_t *storeName)
{
    HANDLE f = CreateFileW(cerPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD size = GetFileSize(f, NULL), got = 0;
    std::vector<BYTE> data(size ? size : 1);
    BOOL ok = ReadFile(f, data.data(), size, &got, NULL);
    CloseHandle(f);
    if (!ok) return GetLastError();
    PCCERT_CONTEXT cert = CertCreateCertificateContext(X509_ASN_ENCODING, data.data(), got);
    if (!cert) {
        /* maybe base64 (.cer exported as text) */
        DWORD bin = 0;
        if (!CryptStringToBinaryA((LPCSTR)data.data(), got, CRYPT_STRING_BASE64HEADER, NULL, &bin, NULL, NULL)) return GetLastError();
        std::vector<BYTE> der(bin);
        CryptStringToBinaryA((LPCSTR)data.data(), got, CRYPT_STRING_BASE64HEADER, der.data(), &bin, NULL, NULL);
        cert = CertCreateCertificateContext(X509_ASN_ENCODING, der.data(), bin);
        if (!cert) return GetLastError();
    }
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    if (!store) { DWORD e = GetLastError(); CertFreeCertificateContext(cert); return e; }
    DWORD err = CertAddCertificateContextToStore(store, cert, CERT_STORE_ADD_REPLACE_EXISTING, NULL) ? ERROR_SUCCESS : GetLastError();
    CertCloseStore(store, 0);
    CertFreeCertificateContext(cert);
    return err;
}

DWORD SetTestSigning(bool enable, std::wstring *detail)
{
    if (detail) detail->clear();
    if (!IsElevated()) return ERROR_ACCESS_DENIED;
    DWORD code = 0;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\bcdedit.exe\" /set {current} testsigning " + (enable ? L"on" : L"off");
    DWORD err = RunAndWait(cmd, code, detail);
    if (err != ERROR_SUCCESS) return err;
    return code == 0 ? ERROR_SUCCESS : code;
}

/* ------------------------------------------------------------------------- */
/* Services                                                                     */
/* ------------------------------------------------------------------------- */

const wchar_t *ServiceStateName(DWORD s)
{
    switch (s) {
    case SERVICE_STOPPED: return L"stopped";
    case SERVICE_START_PENDING: return L"starting";
    case SERVICE_STOP_PENDING: return L"stopping";
    case SERVICE_RUNNING: return L"running";
    case SERVICE_CONTINUE_PENDING: return L"resuming";
    case SERVICE_PAUSE_PENDING: return L"pausing";
    case SERVICE_PAUSED: return L"paused";
    default: return L"not installed";
    }
}

bool QueryService(const wchar_t *name, ServiceInfo &info)
{
    info = ServiceInfo();
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_QUERY_STATUS | SERVICE_QUERY_CONFIG);
    if (!s) { CloseServiceHandle(scm); return false; }
    info.installed = true;
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD need = 0;
    if (QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) info.state = ssp.dwCurrentState;
    need = 0;
    QueryServiceConfigW(s, NULL, 0, &need);
    if (need) {
        std::vector<BYTE> buf(need);
        QUERY_SERVICE_CONFIGW *cfg = (QUERY_SERVICE_CONFIGW *)buf.data();
        if (QueryServiceConfigW(s, cfg, need, &need)) { info.startType = cfg->dwStartType; info.binaryPath = cfg->lpBinaryPathName ? cfg->lpBinaryPathName : L""; }
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return true;
}

static DWORD WaitServiceState(SC_HANDLE s, DWORD want, DWORD waitMs)
{
    DWORD t0 = GetTickCount();
    for (;;) {
        SERVICE_STATUS st = {};
        if (!QueryServiceStatus(s, &st)) return GetLastError();
        if (st.dwCurrentState == want) return ERROR_SUCCESS;
        if (GetTickCount() - t0 > waitMs) return ERROR_SERVICE_REQUEST_TIMEOUT;
        Sleep(200);
    }
}

DWORD StartServiceByName(const wchar_t *name, DWORD waitMs)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_START | SERVICE_QUERY_STATUS);
    if (!s) { DWORD e = GetLastError(); CloseServiceHandle(scm); return e; }
    DWORD err = ERROR_SUCCESS;
    if (!StartServiceW(s, 0, NULL)) { err = GetLastError(); if (err == ERROR_SERVICE_ALREADY_RUNNING) err = ERROR_SUCCESS; }
    if (err == ERROR_SUCCESS) err = WaitServiceState(s, SERVICE_RUNNING, waitMs);
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return err;
}

static DWORD StopServiceHandle(SC_HANDLE service, DWORD waitMs)
{
    SERVICE_STATUS status{};
    if (!QueryServiceStatus(service, &status)) return GetLastError();
    if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
    if (status.dwCurrentState != SERVICE_STOP_PENDING && !ControlService(service, SERVICE_CONTROL_STOP, &status)) {
        DWORD error = GetLastError();
        if (error != ERROR_SERVICE_NOT_ACTIVE && error != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) return error;
        // Another caller can begin shutdown between our query and STOP request.
        // Only a confirmed stopped/stopping state makes the refusal harmless.
        if (!QueryServiceStatus(service, &status)) return GetLastError();
        if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING) return error;
    }
    return WaitServiceState(service, SERVICE_STOPPED, waitMs);
}

DWORD StopServiceByName(const wchar_t *name, DWORD waitMs)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s) { DWORD e = GetLastError(); CloseServiceHandle(scm); return e; }
    DWORD err = StopServiceHandle(s, waitMs);
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return err;
}

DWORD StopServiceAndWaitForExit(const wchar_t *name, DWORD waitMs)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE s = OpenServiceW(scm, name, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s) { DWORD e = GetLastError(); CloseServiceHandle(scm); return e == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : e; }
    SERVICE_STATUS_PROCESS ssp = {};
    DWORD need = 0, pid = 0;
    DWORD err = ERROR_SUCCESS;
    if (!QueryServiceStatusEx(s, SC_STATUS_PROCESS_INFO, (LPBYTE)&ssp, sizeof(ssp), &need)) err = GetLastError();
    // Windows does not guarantee the PID in START_PENDING/STOP_PENDING, and it
    // is never valid in STOPPED. Do not open a stale or unrelated process.
    else if (ssp.dwCurrentState == SERVICE_RUNNING || ssp.dwCurrentState == SERVICE_PAUSED ||
             ssp.dwCurrentState == SERVICE_PAUSE_PENDING || ssp.dwCurrentState == SERVICE_CONTINUE_PENDING) pid = ssp.dwProcessId;
    HANDLE proc = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
    // An already-exited process has no handle; other failures must not be
    // mistaken for evidence that its executable can safely be replaced.
    if (pid && !proc) { DWORD openError = GetLastError(); if (openError != ERROR_INVALID_PARAMETER) err = openError; }
    if (err == ERROR_SUCCESS) err = StopServiceHandle(s, waitMs);
    if (err == ERROR_SUCCESS && proc) {
        /* SERVICE_STOPPED is reported before the process has released its image; wait for the exit */
        DWORD result = WaitForSingleObject(proc, waitMs);
        if (result == WAIT_TIMEOUT) err = ERROR_SERVICE_REQUEST_TIMEOUT;
        else if (result != WAIT_OBJECT_0) err = GetLastError();
    }
    if (proc) CloseHandle(proc);
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return err;
}

DWORD CreateHostService(const std::wstring &exePath)
{
    DWORD validation = ValidateProtectedService(exePath);
    if (validation != ERROR_SUCCESS) return validation;
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CREATE_SERVICE);
    if (!scm) return GetLastError();
    std::wstring cmd = L"\"" + exePath + L"\" --service";
    SC_HANDLE s = CreateServiceW(scm, PN_HOST_SERVICE, PN_HOST_DISPLAY_NAME, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                 SERVICE_ERROR_NORMAL, cmd.c_str(), NULL, NULL, NULL, NULL, NULL);
    DWORD err = ERROR_SUCCESS;
    if (!s) {
        err = GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            s = OpenServiceW(scm, PN_HOST_SERVICE, SERVICE_CHANGE_CONFIG | SERVICE_START);
            if (!s) err = GetLastError();
            else if (ChangeServiceConfigW(s, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, cmd.c_str(), NULL, NULL, NULL, NULL, NULL, PN_HOST_DISPLAY_NAME)) err = ERROR_SUCCESS;
            else err = GetLastError();
        }
    }
    if (s) {
        SERVICE_DESCRIPTIONW d = { (LPWSTR)PN_HOST_DESCRIPTION };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);
        SC_ACTION actions[3] = { { SC_ACTION_RESTART, 2000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 10000 } };
        SERVICE_FAILURE_ACTIONSW fa = { 60, NULL, NULL, 3, actions };
        ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
    return err;
}

DWORD DeleteServiceByName(const wchar_t *name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return GetLastError();
    SC_HANDLE s = OpenServiceW(scm, name, DELETE | SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!s) { DWORD e = GetLastError(); CloseServiceHandle(scm); return e == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : e; }
    DWORD err = StopServiceHandle(s, 10000);
    if (err == ERROR_SUCCESS && !DeleteService(s)) {
        err = GetLastError();
        if (err == ERROR_SERVICE_MARKED_FOR_DELETE) err = ERROR_SUCCESS;
    }
    CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return err;
}

/* ------------------------------------------------------------------------- */
/* Installation                                                                 */
/* ------------------------------------------------------------------------- */

std::wstring ProgramFilesDir()
{
    wchar_t p[MAX_PATH] = {};
    if (SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES, NULL, SHGFP_TYPE_CURRENT, p) == S_OK) return p;
    return L"C:\\Program Files";
}

std::wstring InstallDir()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PN_REG_KEY, L"InstallDir", RRF_RT_REG_SZ, NULL, buf, &size) == ERROR_SUCCESS && buf[0]) return buf;
    return JoinPath(ProgramFilesDir(), L"PN-A2DP");
}

std::vector<PluginInfo> EnumeratePlugins(const std::wstring &dir)
{
    std::vector<PluginInfo> out;
    wchar_t full[MAX_PATH];
    if (!GetFullPathNameW(dir.c_str(), MAX_PATH, full, NULL)) return out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(full, L"pna2dp-codec-*.dll").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        PluginInfo p;
        p.file = JoinPath(full, fd.cFileName);
        HMODULE m = LoadLibraryExW(p.file.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (m) {
            PnCodecPluginGetInfoFn fn = (PnCodecPluginGetInfoFn)GetProcAddress(m, PNA2DP_PLUGIN_ENTRY);
            const PnCodecPlugin *api = fn ? fn() : nullptr;
            if (api && api->api_version == PNA2DP_PLUGIN_API_VERSION) {
                auto wide = [](const char *s) {
                    if (!s) return std::wstring();
                    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
                    std::wstring w(n > 0 ? n - 1 : 0, 0);
                    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, &w[0], n);
                    return w;
                };
                p.valid = true;
                p.name = wide(api->name);
                p.version = wide(api->version);
                p.license = wide(api->license);
                p.codecMask = api->codec_mask;
            }
            FreeLibrary(m);
        } else {
            p.name = L"(failed to load: " + Win32ErrorText(GetLastError()) + L")";
        }
        out.push_back(p);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return out;
}

/* ------------------------------------------------------------------------- */
/* Processes                                                                    */
/* ------------------------------------------------------------------------- */

bool IsElevated()
{
    HANDLE tok = NULL;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    bool r = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(tok);
    return r;
}

DWORD RunElevated(const std::wstring &exe, const std::wstring &params, DWORD &exitCode, HWND owner)
{
    exitCode = 0;
    struct RestoreInput {
        HWND window;
        bool enabled;
        ~RestoreInput() { if (enabled && IsWindow(window)) EnableWindow(window, TRUE); }
    } input{owner, owner && IsWindowEnabled(owner) != FALSE};
    if (input.enabled) EnableWindow(owner, FALSE);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = owner;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return GetLastError();
    if (!sei.hProcess) return ERROR_INVALID_HANDLE;
    DWORD error = ERROR_SUCCESS;
    bool quitting = false;
    WPARAM quitCode = 0;
    for (;;) {
        // The calling thread owns the panel (or COM windows). A plain infinite
        // wait starves messages and can deadlock a helper waiting on a broadcast.
        DWORD wait = MsgWaitForMultipleObjectsEx(1, &sei.hProcess, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) { error = GetLastError(); break; }
        if (wait != WAIT_OBJECT_0 + 1) { error = ERROR_GEN_FAILURE; break; }
        MSG message{};
        // Recheck the process even when another source continuously posts input.
        for (int count = 0; count < 64 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { quitting = true; quitCode = message.wParam; continue; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (!error && !GetExitCodeProcess(sei.hProcess, &exitCode)) error = GetLastError();
    CloseHandle(sei.hProcess);
    if (quitting) PostQuitMessage((int)quitCode);
    return error;
}

DWORD RunAndWait(const std::wstring &cmdLine, DWORD &exitCode, std::wstring *output)
{
    exitCode = 0;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd = NULL, wr = NULL;
    if (output && !CreatePipe(&rd, &wr, &sa, 0)) return GetLastError();
    if (rd) SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW | (output ? STARTF_USESTDHANDLES : 0);
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = cmdLine;
    BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, output ? TRUE : FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (wr) CloseHandle(wr);
    if (ok) {
        if (output) {
            std::string acc;
            char buf[512];
            DWORD n = 0;
            while (ReadFile(rd, buf, sizeof(buf), &n, NULL) && n) acc.append(buf, n);
            int wl = MultiByteToWideChar(CP_OEMCP, 0, acc.c_str(), (int)acc.size(), NULL, 0);
            output->resize(wl);
            if (wl > 0) MultiByteToWideChar(CP_OEMCP, 0, acc.c_str(), (int)acc.size(), &(*output)[0], wl);
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &exitCode);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    if (rd) CloseHandle(rd);
    return err;
}

} // namespace pn
