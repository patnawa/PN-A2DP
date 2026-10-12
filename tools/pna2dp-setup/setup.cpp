/*
 * setup.cpp - PN-A2DP installer / uninstaller.
 *
 * A self-contained Win32 wizard. build.ps1 embeds every deliverable (driver package,
 * codec host, plugins, control panel, command line tool, licenses) as PAYLOAD resources
 * plus a text index, so one pna2dp-setup.exe installs the whole product:
 *
 *   1. components to install (driver, codec host service, LDAC / aptX / AAC plugins,
 *      control panel, command line tool, shortcuts)
 *   2. install folder
 *   3. which paired Bluetooth stereo devices get the PN-A2DP driver (the others keep
 *      the Microsoft inbox driver; the control panel can switch them later)
 *   4. verify Microsoft driver signatures and catalog membership before installing
 *   5. elevation through UAC, then file copy, driver staging and per-device install,
 *      service registration, Start menu shortcuts, Add/Remove Programs entry
 *
 *   pna2dp-setup.exe                         wizard
 *   pna2dp-setup.exe --silent [--dir <folder>] [--no-devices]   unattended install (all components, all devices)
 *   pna2dp-setup.exe --uninstall [--silent]  remove everything, restore the inbox driver on every device
 *   pna2dp-setup.exe --extract <folder>      only unpack the payload (portable use)
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include "resource.h"
#include "pndrv.h"
#include "pnctl.h"
#include "pnsigning.h"
#include "../pna2dp-panel/version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

#define WM_APP_LOG    (WM_APP + 1)   /* lParam = wchar_t* (heap, freed by receiver) */
#define WM_APP_STEP   (WM_APP + 2)   /* wParam = done, lParam = total */
#define WM_APP_DONE   (WM_APP + 3)   /* wParam = error, lParam = reboot */

static HINSTANCE g_inst;
static std::wstring g_selfDeleteDir;   /* set by DoUninstall when this exe lives in the folder being removed */

/* Single-quoted PowerShell literal; paths may contain apostrophes, percent signs or Unicode. */
static std::wstring PsLiteral(const std::wstring &value)
{
    std::wstring result = L"'";
    for (wchar_t c : value) { result += c; if (c == L'\'') result += c; }
    return result + L"'";
}

/* No writable temporary command file, shell path expansion, or delayed deletion of a later reinstall. */
static void ScheduleSelfDelete()
{
    if (g_selfDeleteDir.empty()) return;
    wchar_t pid[16];
    swprintf_s(pid, L"%lu", GetCurrentProcessId());
    std::wstring self = pn::ModulePath();
    std::wstring script = L"Wait-Process -Id " + std::wstring(pid) + L" -ErrorAction SilentlyContinue\n"
        L"Remove-Item -LiteralPath " + PsLiteral(self) + L" -Force -ErrorAction SilentlyContinue\n"
        L"try { [IO.Directory]::Delete(" + PsLiteral(g_selfDeleteDir) + L", $false) } catch {}\n";
    DWORD chars = 0;
    DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringW((const BYTE *)script.data(), (DWORD)(script.size() * sizeof(wchar_t)), flags, NULL, &chars)) return;
    std::wstring encoded(chars, L'\0');
    if (!CryptBinaryToStringW((const BYTE *)script.data(), (DWORD)(script.size() * sizeof(wchar_t)), flags, &encoded[0], &chars)) return;
    encoded.resize(wcslen(encoded.c_str()));
    wchar_t sysdir[MAX_PATH];
    GetSystemDirectoryW(sysdir, MAX_PATH);
    std::wstring cmdExe = pn::JoinPath(sysdir, L"WindowsPowerShell\\v1.0\\powershell.exe");
    std::wstring cmd = L"\"" + cmdExe + L"\" -NoLogo -NoProfile -NonInteractive -EncodedCommand " + encoded;
    STARTUPINFOW si = { sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(cmdExe.c_str(), &cmd[0], NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, sysdir, &si, &pi)) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    else OutputDebugStringW(L"pna2dp-setup: could not start the self-delete helper");
}

/* ------------------------------------------------------------------------- */
/* Payload                                                                      */
/* ------------------------------------------------------------------------- */

struct PayloadItem {
    std::wstring res, file, sub, comp;
    ULONGLONG size = 0;
};

static bool ReadResource(const wchar_t *name, const void **data, DWORD *size)
{
    HRSRC r = FindResourceW(g_inst, name, L"PAYLOAD");
    if (!r) return false;
    HGLOBAL g = LoadResource(g_inst, r);
    if (!g) return false;
    *data = LockResource(g);
    *size = SizeofResource(g_inst, r);
    return *data != nullptr;
}

static std::vector<PayloadItem> ReadPayloadIndex()
{
    std::vector<PayloadItem> out;
    const void *data = nullptr; DWORD size = 0;
    if (!ReadResource(L"PAYLOAD_INDEX", &data, &size)) return out;
    std::string text((const char *)data, size);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t e = text.find_first_of("\r\n", pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        if (line.empty()) continue;
        std::vector<std::string> f;
        size_t p = 0;
        while (true) { size_t b = line.find('|', p); f.push_back(line.substr(p, b == std::string::npos ? std::string::npos : b - p)); if (b == std::string::npos) break; p = b + 1; }
        if (f.size() < 5) continue;
        PayloadItem it;
        auto w = [](const std::string &s) { return std::wstring(s.begin(), s.end()); };
        it.res = w(f[0]); it.file = w(f[1]); it.sub = w(f[2]); it.comp = w(f[3]); it.size = _strtoui64(f[4].c_str(), NULL, 10);
        out.push_back(it);
    }
    return out;
}

static DWORD ExtractItem(const PayloadItem &it, const std::wstring &dest)
{
    const void *data = nullptr; DWORD size = 0;
    if (!ReadResource(it.res.c_str(), &data, &size)) return ERROR_RESOURCE_NAME_NOT_FOUND;
    HANDLE f = INVALID_HANDLE_VALUE;
    DWORD err = ERROR_SUCCESS;
    /* a service or panel that is still exiting may hold the old file for a moment */
    for (int attempt = 0; attempt < 10; attempt++) {
        f = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) break;
        err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED) return err;
        Sleep(500);
    }
    if (f == INVALID_HANDLE_VALUE) return err;
    DWORD w = 0;
    BOOL ok = WriteFile(f, data, size, &w, NULL);
    err = !ok ? GetLastError() : w == size ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    CloseHandle(f);
    return err;
}

/* Turns what the user typed into an absolute, trailing-separator-free folder that is not a
 * drive root. Returns false when the path cannot be used as an installation folder. */
static bool NormalizeInstallDir(std::wstring &dir)
{
    while (!dir.empty() && (dir.back() == L' ' || dir.back() == L'\t')) dir.pop_back();
    size_t b = dir.find_first_not_of(L" \t");
    if (b == std::wstring::npos) return false;
    dir = dir.substr(b);
    if (dir.size() >= 2 && dir.front() == L'"' && dir.back() == L'"') dir = dir.substr(1, dir.size() - 2);
    if (dir.size() < 3 || !iswalpha(dir[0]) || dir[1] != L':' || (dir[2] != L'\\' && dir[2] != L'/')) return false;   /* "C:x" is a drive-relative path */
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(dir.c_str(), MAX_PATH, full, NULL);
    if (!n || n >= MAX_PATH) return false;
    dir = full;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() <= 3) return false;   /* a bare drive root */
    for (wchar_t ch : dir.substr(3)) if (wcschr(L"<>|?*\"", ch)) return false;
    return true;
}

static std::wstring PayloadText(const std::vector<PayloadItem> &items, const wchar_t *file)
{
    for (auto &it : items) {
        if (_wcsicmp(it.file.c_str(), file) != 0) continue;
        const void *data = nullptr; DWORD size = 0;
        if (!ReadResource(it.res.c_str(), &data, &size)) return L"";
        int n = MultiByteToWideChar(CP_UTF8, 0, (const char *)data, size, NULL, 0);
        std::wstring s(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, (const char *)data, size, &s[0], n);
        /* normalise line ends for the edit control */
        std::wstring r;
        for (size_t i = 0; i < s.size(); i++) { if (s[i] == L'\n' && (i == 0 || s[i - 1] != L'\r')) r += L"\r\n"; else r += s[i]; }
        return r;
    }
    return L"";
}

static bool WriteUtf8File(const std::wstring &path, const std::wstring &text)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), NULL, 0, NULL, NULL);
    std::string u8("\xEF\xBB\xBF");
    u8.resize(3 + n);
    if (n) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &u8[3], n, NULL, NULL);
    DWORD w = 0;
    BOOL ok = WriteFile(f, u8.data(), (DWORD)u8.size(), &w, NULL);
    CloseHandle(f);
    return ok != FALSE;
}

static std::wstring PayloadVersion(const std::vector<PayloadItem> &items)
{
    std::wstring inf = PayloadText(items, L"pna2dp.inf");
    size_t p = inf.find(L"DriverVer");
    if (p == std::wstring::npos) return L"" PN_PANEL_VERSION_TEXT;
    size_t c = inf.find(L',', p);
    if (c == std::wstring::npos) return L"" PN_PANEL_VERSION_TEXT;
    size_t e = inf.find_first_of(L"\r\n ", c + 1);
    return inf.substr(c + 1, e == std::wstring::npos ? std::wstring::npos : e - c - 1);
}

static bool PayloadHas(const std::vector<PayloadItem> &items, const wchar_t *comp)
{
    for (auto &it : items) if (it.comp == comp) return true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Answers (what the wizard collected; passed to the elevated instance)        */
/* ------------------------------------------------------------------------- */

enum Comp { C_DRIVER = 0, C_HOST, C_LDAC, C_APTX, C_AAC, C_PANEL, C_CLI, C_STARTMENU, C_DESKTOP, COMP_COUNT };
static const struct { const wchar_t *name; const wchar_t *comp; const wchar_t *desc; } kComps[COMP_COUNT] = {
    { L"PN-A2DP driver", L"driver", L"The kernel driver (pna2dp.sys) that replaces the Microsoft A2DP driver for the headsets you select. Includes the built-in SBC encoder." },
    { L"Codec host service", L"host", L"Windows service that runs the codec plugins outside the kernel. Required for every codec except SBC." },
    { L"LDAC plugin", L"ldac", L"Sony LDAC encoder (330 / 660 / 990 kbps, up to 96 kHz 24-bit). Apache-2.0." },
    { L"aptX / aptX HD / aptX LL plugin", L"aptx", L"aptX family encoders from libfreeaptx. LGPL-2.1+ (freeaptx.dll is a separate DLL)." },
    { L"AAC plugin", L"aac", L"Fraunhofer FDK AAC encoder (AAC-LC, HE-AAC). FDK AAC license." },
    { L"Control panel", L"panel", L"Windows mode: output volume, mute, format and spatial status. PN-A2DP driver mode adds advanced codec controls." },
    { L"Command line tool", L"cli", L"Windows mode: windows list / status / spatial. PN-A2DP driver commands: status, caps, config, set, watch, stats." },
    { L"Start menu shortcuts", L"startmenu", L"Shortcuts for the control panel and the uninstaller." },
    { L"Desktop shortcut", L"desktop", L"A shortcut to the control panel on the desktop." },
};

struct Answers {
    bool comps[COMP_COUNT] = { true, true, true, true, true, true, true, true, false };
    std::wstring folder;
    std::vector<std::wstring> devices;    /* instance ids that get the driver */
    bool allowTestDriver = false;       /* explicit developer CLI opt-in only */

    bool Save(const std::wstring &path) const
    {
        std::wstring t;
        for (int i = 0; i < COMP_COUNT; i++) { t += kComps[i].comp; t += L"="; t += comps[i] ? L"1" : L"0"; t += L"\n"; }
        t += L"folder=" + folder + L"\n";
        for (auto &d : devices) t += L"device=" + d + L"\n";
        t += std::wstring(L"allowtest=") + (allowTestDriver ? L"1" : L"0") + L"\n";
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD w = 0;
        DWORD bytes = (DWORD)(t.size() * sizeof(wchar_t));
        BOOL ok = WriteFile(f, t.c_str(), bytes, &w, NULL);
        CloseHandle(f);
        return ok && w == bytes;
    }
    bool Load(const std::wstring &path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (f == INVALID_HANDLE_VALUE) return false;
        DWORD size = GetFileSize(f, NULL), got = 0;
        if (!size || size > 1024 * 1024 || size % sizeof(wchar_t)) { CloseHandle(f); return false; }
        std::wstring t(size / sizeof(wchar_t), 0);
        BOOL ok = ReadFile(f, &t[0], size, &got, NULL);
        CloseHandle(f);
        if (!ok || got != size) return false;
        folder.clear();
        allowTestDriver = false;
        devices.clear();
        size_t pos = 0;
        while (pos < t.size()) {
            size_t e = t.find(L'\n', pos);
            std::wstring line = t.substr(pos, e == std::wstring::npos ? std::wstring::npos : e - pos);
            pos = e == std::wstring::npos ? t.size() : e + 1;
            size_t eq = line.find(L'=');
            if (eq == std::wstring::npos) continue;
            std::wstring k = line.substr(0, eq), v = line.substr(eq + 1);
            bool matched = false;
            for (int i = 0; i < COMP_COUNT; i++) if (k == kComps[i].comp) { comps[i] = v == L"1"; matched = true; }
            if (matched) continue;
            if (k == L"folder") folder = v;
            else if (k == L"device") devices.push_back(v);
            else if (k == L"allowtest") allowTestDriver = v == L"1";
        }
        return !folder.empty();
    }
};

/* ------------------------------------------------------------------------- */
/* Installation work (runs elevated, on a worker thread or silently)           */
/* ------------------------------------------------------------------------- */

struct Log {
    HWND hwnd = NULL;
    std::wstring text;
    int done = 0, total = 1;
    void operator()(const wchar_t *fmt, ...)
    {
        wchar_t b[1024];
        va_list ap; va_start(ap, fmt); _vsnwprintf_s(b, _TRUNCATE, fmt, ap); va_end(ap);
        text += b; text += L"\r\n";
        if (hwnd) PostMessageW(hwnd, WM_APP_LOG, 0, (LPARAM)_wcsdup(b));
        OutputDebugStringW(b);
    }
    void Step() { done++; if (hwnd) PostMessageW(hwnd, WM_APP_STEP, done, total); }
};

static pn::PackageTrust InspectPayloadTrust(const std::vector<PayloadItem> &items)
{
    pn::PackageTrust result;
    wchar_t tmp[MAX_PATH], id[40]; GUID guid;
    if (!GetTempPathW(_countof(tmp), tmp) || FAILED(CoCreateGuid(&guid))) {
        result.detail = L"Cannot create a temporary package verification folder."; return result;
    }
    StringFromGUID2(guid, id, _countof(id));
    std::wstring folder = pn::JoinPath(tmp, L"pna2dp-verify-" + std::wstring(id));
    if (!CreateDirectoryW(folder.c_str(), NULL)) { result.error = GetLastError(); result.detail = L"Cannot create verification folder."; return result; }
    struct Cleanup {
        std::wstring folder;
        ~Cleanup() {
            for (auto name : {L"pna2dp.inf", L"pna2dp.sys", L"pna2dp.cat"}) DeleteFileW(pn::JoinPath(folder, name).c_str());
            RemoveDirectoryW(folder.c_str());
        }
    } cleanup{folder};
    for (auto name : {L"pna2dp.inf", L"pna2dp.sys", L"pna2dp.cat"}) {
        const PayloadItem *found = nullptr;
        for (const auto &item : items) if (item.comp == L"driver" && item.file == name) {
            if (found) { result.detail = L"Duplicate driver payload entry."; return result; }
            found = &item;
        }
        if (!found) { result.error = ERROR_FILE_NOT_FOUND; result.detail = L"Missing signed driver package file: " + std::wstring(name); return result; }
        result.error = ExtractItem(*found, pn::JoinPath(folder, name));
        if (result.error) { result.detail = L"Cannot extract the driver for verification."; return result; }
    }
    return pn::VerifyDriverPackage(folder);
}

static DWORD CheckDriverPolicy(const pn::PackageTrust &trust, bool allowTest, std::wstring &reason)
{
    reason = trust.detail;
    if (!trust.trusted) return trust.error ? trust.error : ERROR_INVALID_IMAGE_HASH;
    if (trust.microsoftDriver) return ERROR_SUCCESS;
    if (!allowTest) {
        reason = L"This package has no verified Microsoft release driver signature. Normal-mode installation requires a Microsoft-signed package from Partner Center. "
                 L"The bundled development driver cannot run in normal mode. Setup has not changed Windows security settings.";
        return ERROR_INVALID_IMAGE_HASH;
    }
    auto state = pn::QuerySigningState();
    if (!state.queried || !state.testSigning) {
        reason = L"Developer installation requires test signing to be active already and a trusted driver signature. Prepare the development machine separately, restart, then retry. Setup does not change boot policy.";
        return ERROR_INVALID_IMAGE_HASH;
    }
    reason = L"Developer installation explicitly requested; trusted package verified and test signing is already active.";
    return ERROR_SUCCESS;
}

static DWORD CheckPayload(const Answers &a, const std::vector<PayloadItem> &items, Log &log)
{
    if (!a.comps[C_DRIVER]) return ERROR_SUCCESS;
    auto trust = InspectPayloadTrust(items);
    std::wstring reason;
    DWORD err = CheckDriverPolicy(trust, a.allowTestDriver, reason);
    log(L"Driver verification: %s", reason.c_str());
    if (!trust.signer.empty()) log(L"Catalog signer: %s", trust.signer.c_str());
    if (err) log(L"Preflight failed (0x%08lX). No installation files, services, certificates, boot settings or devices were changed.", err);
    return err;
}

static DWORD CreateShortcut(const std::wstring &lnk, const std::wstring &target, const std::wstring &args, const std::wstring &desc)
{
    IShellLinkW *sl = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void **)&sl);
    if (FAILED(hr)) return (DWORD)hr;
    sl->SetPath(target.c_str());
    sl->SetArguments(args.c_str());
    sl->SetDescription(desc.c_str());
    std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    sl->SetWorkingDirectory(dir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile *pf = nullptr;
    hr = sl->QueryInterface(IID_IPersistFile, (void **)&pf);
    if (SUCCEEDED(hr)) { hr = pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
    return SUCCEEDED(hr) ? ERROR_SUCCESS : (DWORD)hr;
}

static std::wstring StartMenuDir()
{
    wchar_t p[MAX_PATH] = {};
    SHGetFolderPathW(NULL, CSIDL_COMMON_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, p);
    return pn::JoinPath(p, L"PN-A2DP");
}

static std::wstring DesktopDir()
{
    wchar_t p[MAX_PATH] = {};
    SHGetFolderPathW(NULL, CSIDL_COMMON_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, p);
    return p;
}

static void RegSetString(HKEY root, const wchar_t *key, const wchar_t *name, const std::wstring &value)
{
    HKEY h = NULL;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_SZ, (const BYTE *)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(h);
}

static void RegSetDword(HKEY root, const wchar_t *key, const wchar_t *name, DWORD value)
{
    HKEY h = NULL;
    if (RegCreateKeyExW(root, key, 0, NULL, 0, KEY_WRITE | KEY_WOW64_64KEY, NULL, &h, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(h, name, 0, REG_DWORD, (const BYTE *)&value, sizeof(value));
    RegCloseKey(h);
}

static DWORD DoInstall(const Answers &a, const std::vector<PayloadItem> &items, Log &log, bool &reboot)
{
    reboot = false;
    DWORD preflight = CheckPayload(a, items, log);
    if (preflight != ERROR_SUCCESS) return preflight;
    DWORD firstError = ERROR_SUCCESS;
    auto fail = [&](DWORD e) { if (firstError == ERROR_SUCCESS) firstError = e; };
    std::wstring version = PayloadVersion(items);

    /* count steps for the progress bar */
    log.total = 2;
    for (auto &it : items) { bool sel = it.comp == L"core"; for (int i = 0; i < COMP_COUNT; i++) if (a.comps[i] && it.comp == kComps[i].comp) sel = true; if (sel) log.total++; }
    if (a.comps[C_DRIVER]) log.total += 2 + (int)a.devices.size();
    if (a.comps[C_HOST]) log.total += 2;
    if (a.comps[C_STARTMENU] || a.comps[C_DESKTOP]) log.total++;
    log.total += 2;

    log(L"Installing PN-A2DP %s to %s", version.c_str(), a.folder.c_str());
    DWORD err = pn::CreateDirectoryRecursive(a.folder);
    if (err == ERROR_SUCCESS && a.comps[C_DRIVER]) err = pn::CreateDirectoryRecursive(pn::JoinPath(a.folder, L"driver"));
    if (err != ERROR_SUCCESS) { log(L"Cannot create the folder: %s", pn::Win32ErrorText(err).c_str()); return err; }
    log.Step();

    /* stop the service and anything that holds our files before overwriting */
    pn::ServiceInfo si;
    const bool hostWasRunning = a.comps[C_HOST] && pn::QueryService(PN_HOST_SERVICE, si) && si.state != SERVICE_STOPPED;
    struct RestoreHost {
        bool restart;
        ~RestoreHost() {
            pn::ServiceInfo current;
            if (restart && pn::QueryService(PN_HOST_SERVICE, current) && current.state == SERVICE_STOPPED)
                pn::StartServiceByName(PN_HOST_SERVICE);
        }
    } restoreHost{hostWasRunning};
    if (hostWasRunning) {
        log(L"Stopping the codec host service");
        err = pn::StopServiceAndWaitForExit(PN_HOST_SERVICE);
        if (err != ERROR_SUCCESS) { log(L"The running codec host service could not be stopped: %s", pn::Win32ErrorText(err).c_str()); return err; }
    }
    log.Step();

    /* Detect open applications before replacing any payload file. In particular,
     * the panel may have unsaved settings; leave closing it to the user. */
    for (auto &it : items) {
        bool sel = it.comp == L"core";
        for (int i = 0; i < COMP_COUNT; i++) if (a.comps[i] && it.comp == kComps[i].comp) sel = true;
        if (!sel) continue;
        std::wstring dest = pn::JoinPath(it.sub.empty() ? a.folder : pn::JoinPath(a.folder, it.sub), it.file);
        HANDLE file = CreateFileW(dest.c_str(), GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); continue; }
        err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) continue;
        log(L"Cannot replace %s: %s. Close the control panel and other PN-A2DP applications, then retry. No files were replaced.", dest.c_str(), pn::Win32ErrorText(err).c_str());
        return err;
    }

    /* files */
    for (auto &it : items) {
        bool sel = it.comp == L"core";
        for (int i = 0; i < COMP_COUNT; i++) if (a.comps[i] && it.comp == kComps[i].comp) sel = true;
        if (!sel) continue;
        std::wstring dest = pn::JoinPath(it.sub.empty() ? a.folder : pn::JoinPath(a.folder, it.sub), it.file);
        err = ExtractItem(it, dest);
        if (err != ERROR_SUCCESS) {
            log(L"Failed to write %s: %s. Installation stopped before activating the package; retry setup to repair the files.", dest.c_str(), pn::Win32ErrorText(err).c_str());
            return err;
        }
        else log(L"Copied %s", dest.c_str());
        log.Step();
    }

    /* driver */
    bool driverOk = false;
    if (a.comps[C_DRIVER]) {
        std::wstring inf = pn::JoinPath(a.folder, L"driver\\pna2dp.inf");
        log.Step();
        pn::StagedPackage sp;
        err = pn::StagePackage(inf, sp);
        if (err != ERROR_SUCCESS) {
            log(L"Staging the driver package failed: %s", pn::Win32ErrorText(err).c_str());
            if (err == ERROR_REVISION_MISMATCH)
                log(L"  Windows reused a different driver package. Increase DriverVer, rebuild and sign the package before retrying.");
            if (err == 0xE000022F || err == 0xE0000247)
                log(L"  The package carries no (valid) signature. 64-bit Windows refuses it; build with build.ps1 -Sign (test signing) or use a signed release.");
            else if (err == 0xE0000244 || err == 0xE0000245 || err == 0x800B0109 || err == ERROR_DRIVER_BLOCKED)
                log(L"  The package is not trusted by this machine. Test-signed builds need test signing enabled and the test certificate installed; see docs/04-build-sign-install.md.");
            log(L"  The remaining components are installed; stage the driver later from the control panel (Driver tab) once the package is trusted.");
            fail(err);
        } else {
            log(L"Driver package staged as %s (version %s)", sp.publishedName.c_str(), sp.version.c_str());
            driverOk = true;
        }
        log.Step();
        for (auto &dev : a.devices) {
            if (!driverOk) { log.Step(); continue; }
            pn::A2dpNode n;
            std::wstring name = pn::GetA2dpNode(dev, n) && !n.name.empty() ? n.name : dev;
            bool rb = false;
            err = pn::InstallInfOnDevice(dev, sp.fullPath, rb);
            if (err != ERROR_SUCCESS) { log(L"  %s: switching to PN-A2DP failed: %s", name.c_str(), pn::Win32ErrorText(err).c_str()); fail(err); }
            else { log(L"  %s: now uses PN-A2DP%s", name.c_str(), rb ? L" (restart required)" : L""); reboot |= rb; }
            log.Step();
        }
        if (driverOk && a.devices.empty()) log(L"No device was selected; switch headsets to PN-A2DP in the control panel (Driver tab)");
    }

    /* service */
    if (a.comps[C_HOST]) {
        std::wstring exe = pn::JoinPath(a.folder, L"pna2dp-codechost.exe");
        if (!pn::FileExists(exe)) {
            log(L"pna2dp-codechost.exe was not written; the service is not registered");
            fail(ERROR_FILE_NOT_FOUND);
            log.Step(); log.Step();
        } else {
            err = pn::CreateHostService(exe);
            if (err != ERROR_SUCCESS) { log(L"Registering the codec host service failed: %s", pn::Win32ErrorText(err).c_str()); fail(err); }
            else log(L"Codec host service registered (automatic start)");
            log.Step();
            if (err == ERROR_SUCCESS) {
                err = pn::StartServiceByName(PN_HOST_SERVICE);
                if (err != ERROR_SUCCESS) { log(L"Starting the codec host service failed: %s", pn::Win32ErrorText(err).c_str()); fail(err); }
                else log(L"Codec host service running");
            }
            log.Step();
        }
    }

    /* shortcuts */
    std::wstring panel = pn::JoinPath(a.folder, L"pna2dp-panel.exe");
    std::wstring uninst = pn::JoinPath(a.folder, L"uninstall.exe");
    if (a.comps[C_STARTMENU] || a.comps[C_DESKTOP]) {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        if (a.comps[C_STARTMENU]) {
            std::wstring dir = StartMenuDir();
            pn::CreateDirectoryRecursive(dir);
            if (a.comps[C_PANEL]) CreateShortcut(pn::JoinPath(dir, L"PN-A2DP Control Panel.lnk"), panel, L"", L"Configure Bluetooth audio codecs");
            CreateShortcut(pn::JoinPath(dir, L"Uninstall PN-A2DP.lnk"), uninst, L"--uninstall", L"Remove PN-A2DP and restore the Microsoft driver");
            log(L"Start menu shortcuts created");
        }
        if (a.comps[C_DESKTOP] && a.comps[C_PANEL]) { CreateShortcut(pn::JoinPath(DesktopDir(), L"PN-A2DP Control Panel.lnk"), panel, L"", L"Configure Bluetooth audio codecs"); log(L"Desktop shortcut created"); }
        CoUninitialize();
        log.Step();
    }

    /* uninstaller + registry */
    std::wstring self = pn::ModulePath();
    if (_wcsicmp(self.c_str(), uninst.c_str()) != 0 && !CopyFileW(self.c_str(), uninst.c_str(), FALSE)) {
        DWORD e = GetLastError();
        log(L"Could not copy the uninstaller: %s", pn::Win32ErrorText(e).c_str());
        fail(e);
    }
    RegSetString(HKEY_LOCAL_MACHINE, PN_REG_KEY, L"InstallDir", a.folder);
    RegSetString(HKEY_LOCAL_MACHINE, PN_REG_KEY, L"Version", version);
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"DisplayName", L"PN-A2DP Bluetooth audio driver");
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"DisplayVersion", version);
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"Publisher", PN_INF_PROVIDER);
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"InstallLocation", a.folder);
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"UninstallString", L"\"" + uninst + L"\" --uninstall");
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"QuietUninstallString", L"\"" + uninst + L"\" --uninstall --silent");
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"DisplayIcon", a.comps[C_PANEL] ? panel : uninst);
    RegSetString(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"URLInfoAbout", L"https://github.com/patnawa/PN-A2DP");
    RegSetDword(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"NoModify", 1);
    RegSetDword(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"NoRepair", 1);
    ULONGLONG bytes = 0; for (auto &it : items) bytes += it.size;
    RegSetDword(HKEY_LOCAL_MACHINE, PN_ARP_KEY, L"EstimatedSize", (DWORD)(bytes / 1024));
    log(L"Registered in Apps & features");
    log.Step();
    log.Step();
    log(firstError == ERROR_SUCCESS ? L"Installation complete." : L"Installation finished with errors.");
    return firstError;
}

static DWORD DoUninstall(Log &log, bool &reboot)
{
    reboot = false;
    DWORD firstError = ERROR_SUCCESS;
    auto fail = [&](DWORD e) { if (firstError == ERROR_SUCCESS) firstError = e; };
    std::wstring dir = pn::InstallDir();
    DWORD enumerationError = ERROR_SUCCESS;
    auto nodes = pn::EnumerateA2dpSinkNodes(&enumerationError);
    if (enumerationError != ERROR_SUCCESS) {
        log(L"Removal stopped: cannot inspect Bluetooth devices: %s. Application files and drivers have been retained.", pn::Win32ErrorText(enumerationError).c_str());
        return enumerationError;
    }
    auto staged = pn::FindStagedPackages();
    int ours = 0;
    for (auto &n : nodes) if (n.usesPnA2dp()) ours++;
    log.total = ours + (int)staged.size() + 4;

    for (auto &n : nodes) {
        if (!n.usesPnA2dp()) continue;
        bool rb = false;
        DWORD err = pn::RestoreInboxDriver(n.instanceId, rb);
        if (err != ERROR_SUCCESS) { log(L"%s: restoring the Microsoft driver failed: %s", n.name.c_str(), pn::Win32ErrorText(err).c_str()); fail(err); }
        else { log(L"%s: Microsoft driver restored%s", n.name.c_str(), rb ? L" (restart required)" : L""); reboot |= rb; }
        log.Step();
    }
    if (firstError != ERROR_SUCCESS) {
        log(L"Removal stopped: restore the affected devices before removing the package or application files.");
        return firstError;
    }
    for (auto &p : staged) {
        DWORD err = pn::UnstagePackage(p.publishedName);
        if (err != ERROR_SUCCESS) { log(L"Removing %s from the driver store failed: %s", p.publishedName.c_str(), pn::Win32ErrorText(err).c_str()); fail(err); }
        else log(L"Driver package %s removed from the driver store", p.publishedName.c_str());
        log.Step();
    }
    DWORD err = pn::DeleteServiceByName(PN_HOST_SERVICE);
    if (err != ERROR_SUCCESS) { log(L"Removing the codec host service failed: %s", pn::Win32ErrorText(err).c_str()); fail(err); }
    else log(L"Codec host service removed");
    log.Step();

    if (firstError != ERROR_SUCCESS) {
        log(L"Removal stopped: application files and uninstall registration have been retained so removal can be retried.");
        return firstError;
    }
    std::wstring sm = StartMenuDir();
    DeleteFileW(pn::JoinPath(sm, L"PN-A2DP Control Panel.lnk").c_str());
    DeleteFileW(pn::JoinPath(sm, L"Uninstall PN-A2DP.lnk").c_str());
    RemoveDirectoryW(sm.c_str());
    DeleteFileW(pn::JoinPath(DesktopDir(), L"PN-A2DP Control Panel.lnk").c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PN_ARP_KEY);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, PN_REG_KEY);
    log(L"Shortcuts and registry entries removed");
    log.Step();

    /* files: everything we know about; the uninstaller itself is deleted after exit */
    const wchar_t *names[] = { L"pna2dp-codechost.exe", L"pna2dp-panel.exe", L"pna2dpctl.exe", L"freeaptx.dll", L"LICENSE", L"THIRD-PARTY-NOTICES.md", L"README.md",
                               L"13-windows-mode.md", L"pna2dp-setup.log", L"driver\\pna2dp.inf", L"driver\\pna2dp.sys", L"driver\\pna2dp.cat", L"driver\\pna2dp-test.cer" };
    for (auto n : names) { std::wstring p = pn::JoinPath(dir, n); if (pn::FileExists(p) && !DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT); }
    /* the uninstaller copy, unless it is the running module (then ScheduleSelfDelete removes it) */
    std::wstring uninstCopy = pn::JoinPath(dir, L"uninstall.exe");
    if (_wcsicmp(uninstCopy.c_str(), pn::ModulePath().c_str()) != 0 && pn::FileExists(uninstCopy) && !DeleteFileW(uninstCopy.c_str())) MoveFileExW(uninstCopy.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pn::JoinPath(dir, L"pna2dp-codec-*.dll").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do { std::wstring p = pn::JoinPath(dir, fd.cFileName); if (!DeleteFileW(p.c_str())) MoveFileExW(p.c_str(), NULL, MOVEFILE_DELAY_UNTIL_REBOOT); } while (FindNextFileW(h, &fd)); FindClose(h); }
    RemoveDirectoryW(pn::JoinPath(dir, L"driver").c_str());
    log(L"Files removed from %s", dir.c_str());
    log.Step();

    /* uninstall.exe (this process) and the folder are deleted by ScheduleSelfDelete() right before the process exits */
    std::wstring self = pn::ModulePath();
    if (self.size() > dir.size() && self[dir.size()] == L'\\' && _wcsnicmp(self.c_str(), dir.c_str(), dir.size()) == 0) g_selfDeleteDir = dir;
    log.Step();
    log(firstError == ERROR_SUCCESS ? L"PN-A2DP has been removed. Windows reinstalls the Microsoft driver for any headset that still showed PN-A2DP when it next connects." : L"Removal finished with errors.");
    return firstError;
}

/* ------------------------------------------------------------------------- */
/* Wizard                                                                       */
/* ------------------------------------------------------------------------- */

enum Mode { M_INSTALL, M_ELEVATED_INSTALL, M_UNINSTALL, M_ELEVATED_UNINSTALL };

class Wizard {
public:
    Wizard(Mode mode, const std::vector<PayloadItem> &items, const Answers &answers) : m_mode(mode), m_items(items), m_a(answers) {}
    int Run();
    static INT_PTR CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);

private:
    enum Pg { P_WELCOME, P_LICENSE, P_COMPONENTS, P_FOLDER, P_DEVICES, P_SIGNING, P_PROGRESS, P_FINISH, P_UNINSTALL, P_COUNT };
    Mode m_mode;
    std::vector<PayloadItem> m_items;
    Answers m_a;
    HWND m_hwnd = NULL;
    HWND m_page[P_COUNT] = {};
    int m_cur = -1;
    std::vector<pn::A2dpNode> m_nodes;
    pn::PackageTrust m_trust;
    std::wstring ComponentDescription(int i) const
    {
        std::wstring d = kComps[i].desc;
        if (i == C_DRIVER) {
            std::wstring reason;
            CheckDriverPolicy(m_trust, m_a.allowTestDriver, reason);
            d += L"\r\n\r\n" + reason;
        }
        return d;
    }
    std::thread m_worker;
    Log m_log;
    DWORD m_result = ERROR_SUCCESS;
    bool m_reboot = false;
    bool m_running = false;

    void OnInit();
    void Show(int p);
    bool Leave(int p);           /* validate + collect */
    void Enter(int p);
    int NextOf(int p);
    int PrevOf(int p);
    void StartWork();
    void OnDone(DWORD err, bool reboot);
    void Header(const wchar_t *title, const wchar_t *sub) { SetDlgItemTextW(m_hwnd, IDC_WZ_TITLE, title); SetDlgItemTextW(m_hwnd, IDC_WZ_SUBTITLE, sub); }
    friend INT_PTR CALLBACK WizPageThunk(HWND, UINT, WPARAM, LPARAM);
    INT_PTR OnPageCommand(HWND page, int id, int code);
    INT_PTR OnPageNotify(HWND page, NMHDR *nm);
};

static Wizard *g_wiz;

int Wizard::Run()
{
    g_wiz = this;
    return (int)DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_WIZARD), NULL, Proc, (LPARAM)this);
}

INT_PTR CALLBACK Wizard::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Wizard *w = g_wiz;
    switch (msg) {
    case WM_INITDIALOG: w->m_hwnd = hwnd; w->OnInit(); return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_WZ_NEXT: {
            if (w->m_cur == P_FINISH) {
                if (IsDlgButtonChecked(w->m_page[P_FINISH], IDC_FINISH_LAUNCH) == BST_CHECKED) {
                    std::wstring panel = pn::JoinPath(w->m_a.folder, L"pna2dp-panel.exe");
                    /* launch unelevated through the shell so the panel runs as the logged-on user */
                    ShellExecuteW(NULL, L"open", L"explorer.exe", (L"\"" + panel + L"\"").c_str(), NULL, SW_SHOWNORMAL);
                }
                if (IsDlgButtonChecked(w->m_page[P_FINISH], IDC_FINISH_REBOOT) == BST_CHECKED) {
                    HANDLE tok; TOKEN_PRIVILEGES tp = { 1 };
                    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) {
                        LookupPrivilegeValueW(NULL, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid); tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                        AdjustTokenPrivileges(tok, FALSE, &tp, 0, NULL, NULL); CloseHandle(tok);
                    }
                    ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_SOFTWARE | SHTDN_REASON_MINOR_INSTALLATION | SHTDN_REASON_FLAG_PLANNED);
                }
                EndDialog(hwnd, (int)w->m_result);
                return TRUE;
            }
            if (!w->Leave(w->m_cur)) return TRUE;
            int n = w->NextOf(w->m_cur);
            if (n == P_PROGRESS && !pn::IsElevated()) {
                /* hand over to an elevated copy of ourselves */
                wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
                wchar_t answerPath[MAX_PATH];
                if (!GetTempFileNameW(tmp, L"pna", 0, answerPath)) return TRUE;
                std::wstring answers = answerPath;
                std::wstring params;
                if (w->m_mode == M_UNINSTALL) params = L"--uninstall --elevated";
                else {
                    if (!w->m_a.Save(answers)) { DeleteFileW(answers.c_str()); MessageBoxW(hwnd, L"Cannot save setup selections.", L"PN-A2DP Setup", MB_ICONERROR); return TRUE; }
                    params = L"--elevated-install \"" + answers + L"\"";
                }
                if (w->m_mode == M_UNINSTALL) DeleteFileW(answers.c_str());
                std::wstring self = pn::ModulePath();
                SHELLEXECUTEINFOW sei = { sizeof(sei) };
                sei.fMask = SEE_MASK_FLAG_NO_UI; sei.hwnd = hwnd; sei.lpVerb = L"runas"; sei.lpFile = self.c_str(); sei.lpParameters = params.c_str(); sei.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExW(&sei)) {
                    DWORD e = GetLastError();
                    DeleteFileW(answers.c_str());
                    if (e != ERROR_CANCELLED) MessageBoxW(hwnd, (L"Cannot start the elevated installer: " + pn::Win32ErrorText(e)).c_str(), L"PN-A2DP Setup", MB_ICONERROR);
                    return TRUE;
                }
                EndDialog(hwnd, 0);
                return TRUE;
            }
            w->Show(n);
            if (n == P_PROGRESS) w->StartWork();     /* already elevated: no hand-off, run the work here */
            return TRUE;
        }
        case IDC_WZ_BACK: if (w->m_cur > 0) w->Show(w->PrevOf(w->m_cur)); return TRUE;
        case IDCANCEL:
            if (w->m_running) return TRUE;
            if (w->m_cur != P_FINISH && w->m_cur != P_PROGRESS && MessageBoxW(hwnd, L"Exit setup?", L"PN-A2DP Setup", MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            EndDialog(hwnd, ERROR_CANCELLED);
            return TRUE;
        }
        break;
    case WM_APP_LOG: {
        wchar_t *s = (wchar_t *)lp;
        HWND lb = GetDlgItem(w->m_page[P_PROGRESS], IDC_PROG_LOG);
        int i = ListBox_AddString(lb, s);
        ListBox_SetTopIndex(lb, i);
        SetDlgItemTextW(w->m_page[P_PROGRESS], IDC_PROG_STEP, s);
        free(s);
        return TRUE;
    }
    case WM_APP_STEP: SendMessageW(GetDlgItem(w->m_page[P_PROGRESS], IDC_PROG_BAR), PBM_SETRANGE32, 0, (LPARAM)lp); SendMessageW(GetDlgItem(w->m_page[P_PROGRESS], IDC_PROG_BAR), PBM_SETPOS, wp, 0); return TRUE;
    case WM_APP_DONE: w->OnDone((DWORD)wp, lp != 0); return TRUE;
    case WM_CLOSE: if (!w->m_running) SendMessageW(hwnd, WM_COMMAND, IDCANCEL, 0); return TRUE;
    }
    return FALSE;
}

void Wizard::OnInit()
{
    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0));
    /* bold title */
    HFONT f = (HFONT)SendMessageW(m_hwnd, WM_GETFONT, 0, 0);
    LOGFONTW lf; GetObjectW(f, sizeof(lf), &lf); lf.lfWeight = FW_BOLD; lf.lfHeight = lf.lfHeight * 12 / 10;
    SendMessageW(GetDlgItem(m_hwnd, IDC_WZ_TITLE), WM_SETFONT, (WPARAM)CreateFontIndirectW(&lf), TRUE);

    static const int dlg[P_COUNT] = { IDD_WZ_WELCOME, IDD_WZ_LICENSE, IDD_WZ_COMPONENTS, IDD_WZ_FOLDER, IDD_WZ_DEVICES, IDD_WZ_SIGNING, IDD_WZ_PROGRESS, IDD_WZ_FINISH, IDD_WZ_UNINSTALL };
    RECT rc = { 7, 40, 393, 250 };
    MapDialogRect(m_hwnd, &rc);
    for (int i = 0; i < P_COUNT; i++) {
        m_page[i] = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(dlg[i]), m_hwnd, PageProc, i);
        SetWindowPos(m_page[i], NULL, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    std::wstring version = PayloadVersion(m_items);
    if (m_mode != M_UNINSTALL && m_mode != M_ELEVATED_UNINSTALL && PayloadHas(m_items, L"driver")) m_trust = InspectPayloadTrust(m_items);
    std::wstring plugins;
    if (PayloadHas(m_items, L"ldac")) plugins += L"LDAC, ";
    if (PayloadHas(m_items, L"aptx")) plugins += L"aptX / aptX HD / aptX LL, ";
    if (PayloadHas(m_items, L"aac")) plugins += L"AAC, ";
    plugins += L"SBC";
    wchar_t text[2048];
    swprintf_s(text,
        L"This wizard installs PN-A2DP %s, an open Bluetooth A2DP audio driver for Windows 10 and 11.\r\n\r\n"
        L"What you get:\r\n"
        L"  \x2022  a replacement for the Microsoft A2DP driver on the headsets you choose; the others keep the inbox driver\r\n"
        L"  \x2022  selectable codecs with full control over their parameters: %s\r\n"
        L"  \x2022  live reconfiguration, adaptive bitrate, latency reporting and a control panel\r\n\r\n"
        L"Requirements: Windows 10 2004 or Windows 11, x64, administrator rights. The headset must already be paired in Windows Settings.\r\n\r\n"
        L"%s\r\n\r\nClick Next to continue.",
        version.c_str(), plugins.c_str(),
        m_trust.microsoftDriver ? L"Microsoft driver signature verified. Setup uses normal Windows driver installation." : L"This build is not ready for normal-mode driver installation. A Microsoft-signed release package is required; Setup will not enable test mode.");
    if (!PayloadHas(m_items, L"driver")) swprintf_s(text,
        L"PN-A2DP %s - Windows mode\r\n\r\nManage Bluetooth audio using the Windows driver already on your PC.\r\n\r\n"
        L"Output volume and mute, live output information, Windows mix format, and spatial sound detection.\r\n\r\n"
        L"Windows chooses the Bluetooth codec and bitrate. This package contains no PN-A2DP kernel driver or codec host. Test Mode and a custom certificate are not required.\r\n\r\n"
        L"Existing driver assignments and boot settings are preserved. You can select Use Windows mode on the panel's Driver tab.\r\n\r\nClick Next to continue.", version.c_str());
    SetDlgItemTextW(m_page[P_WELCOME], IDC_WELCOME_TEXT, text);

    std::wstring lic = PayloadText(m_items, L"LICENSE");
    std::wstring notices = PayloadText(m_items, L"THIRD-PARTY-NOTICES.md");
    SetDlgItemTextW(m_page[P_LICENSE], IDC_LICENSE_TEXT, (lic + L"\r\n\r\n---- Third-party notices ----\r\n\r\n" + notices).c_str());

    HWND lv = GetDlgItem(m_page[P_COMPONENTS], IDC_COMP_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW c = {}; c.mask = LVCF_WIDTH; c.cx = 280; ListView_InsertColumn(lv, 0, &c);
    for (int i = 0; i < COMP_COUNT; i++) {
        LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = i; it.pszText = (LPWSTR)kComps[i].name; it.lParam = i;
        ListView_InsertItem(lv, &it);
        bool available = true;
        if (i == C_LDAC || i == C_APTX || i == C_AAC || i == C_DRIVER || i == C_HOST || i == C_PANEL || i == C_CLI) available = PayloadHas(m_items, kComps[i].comp);
        if (!available) { m_a.comps[i] = false; std::wstring label = std::wstring(kComps[i].name) + L" (not in this package)"; ListView_SetItemText(lv, i, 0, (LPWSTR)label.c_str()); }
        if (i == C_DRIVER && available && !m_trust.microsoftDriver && !m_a.allowTestDriver) {
            std::wstring label = std::wstring(kComps[i].name) + L" (Microsoft signing required)";
            ListView_SetItemText(lv, i, 0, (LPWSTR)label.c_str());
        }
        ListView_SetCheckState(lv, i, m_a.comps[i] && available);
    }
    SetDlgItemTextW(m_page[P_COMPONENTS], IDC_COMP_DESC, ComponentDescription(0).c_str());

    SetDlgItemTextW(m_page[P_FOLDER], IDC_FOLDER_EDIT, m_a.folder.c_str());
    SetDlgItemTextW(m_page[P_FOLDER], IDC_FOLDER_NOTE, L"The driver package is copied to a \"driver\" sub-folder and staged in the Windows driver store from there. The codec host service and the plugins run from this folder; extra plugins can also be dropped into %ProgramData%\\PN-A2DP\\plugins.");

    lv = GetDlgItem(m_page[P_DEVICES], IDC_DEV_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
    LVCOLUMNW cols[4] = {};
    if (!PayloadHas(m_items, L"driver")) SetDlgItemTextW(m_page[P_FOLDER], IDC_FOLDER_NOTE,
        L"Installs the control panel and command-line tool. Uses the existing Windows audio driver; no codec service, test certificate or boot change is needed.");
    const wchar_t *names[4] = { L"Device", L"Address", L"Current driver", L"State" };
    int widths[4] = { 230, 120, 120, 80 };
    for (int i = 0; i < 4; i++) { cols[i].mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM; cols[i].pszText = (LPWSTR)names[i]; cols[i].cx = widths[i]; cols[i].iSubItem = i; ListView_InsertColumn(lv, i, &cols[i]); }
    m_nodes = pn::EnumerateA2dpSinkNodes();
    for (size_t i = 0; i < m_nodes.size(); i++) {
        LVITEMW it = {}; it.mask = LVIF_TEXT | LVIF_PARAM; it.iItem = (int)i; it.pszText = (LPWSTR)m_nodes[i].name.c_str(); it.lParam = (LPARAM)i;
        ListView_InsertItem(lv, &it);
        std::wstring addr = pn::BtAddressText(m_nodes[i].address);
        ListView_SetItemText(lv, (int)i, 1, (LPWSTR)addr.c_str());
        ListView_SetItemText(lv, (int)i, 2, (LPWSTR)(m_nodes[i].usesPnA2dp() ? L"PN-A2DP" : m_nodes[i].driverProvider.empty() ? L"none" : m_nodes[i].driverProvider.c_str()));
        ListView_SetItemText(lv, (int)i, 3, (LPWSTR)(!m_nodes[i].present ? L"not present" : m_nodes[i].connected ? L"connected" : L"paired"));
        ListView_SetCheckState(lv, (int)i, TRUE);
    }
    SetDlgItemTextW(m_page[P_DEVICES], IDC_DEV_NOTE, m_nodes.empty() ? L"No paired Bluetooth stereo device was found. Pair a headset first, or finish the installation and switch devices later in the control panel." :
                    L"Switching a device recreates its audio endpoint; Windows may ask for a restart for a device that is currently streaming.");

    std::wstring signingReason;
    CheckDriverPolicy(m_trust, m_a.allowTestDriver, signingReason);
    SetDlgItemTextW(m_page[P_SIGNING], IDC_SIGN_INFO, signingReason.c_str());
    SetDlgItemTextW(m_page[P_SIGNING], IDC_SIGN_NOTE,
        L"Setup verifies the catalog signature and the INF and SYS hashes before changing installed files. "
        L"Secure Boot, memory integrity and test-signing settings are left as configured.\r\n\r\n"
        L"Microsoft signing establishes driver trust; the device must also meet Windows compatibility and security policies. "
        L"A development package needs separate developer setup and the explicit --allow-test-driver option.");

    Show(m_mode == M_UNINSTALL || m_mode == M_ELEVATED_UNINSTALL ? P_UNINSTALL : P_WELCOME);
    if (m_mode == M_ELEVATED_INSTALL || m_mode == M_ELEVATED_UNINSTALL) { Show(P_PROGRESS); StartWork(); }
}

INT_PTR CALLBACK Wizard::PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Wizard *w = g_wiz;
    switch (msg) {
    case WM_INITDIALOG: return TRUE;
    case WM_COMMAND: return w->OnPageCommand(hwnd, LOWORD(wp), HIWORD(wp));
    case WM_NOTIFY: return w->OnPageNotify(hwnd, (NMHDR *)lp);
    }
    return FALSE;
}

INT_PTR Wizard::OnPageCommand(HWND page, int id, int)
{
    if (page == m_page[P_LICENSE] && id == IDC_LICENSE_ACCEPT) { EnableWindow(GetDlgItem(m_hwnd, IDC_WZ_NEXT), IsDlgButtonChecked(page, IDC_LICENSE_ACCEPT) == BST_CHECKED); return TRUE; }
    if (page == m_page[P_FOLDER] && id == IDC_FOLDER_BROWSE) {
        IFileDialog *fd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_IFileDialog, (void **)&fd))) {
            DWORD opts = 0; fd->GetOptions(&opts); fd->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            fd->SetTitle(L"Select the installation folder");
            if (SUCCEEDED(fd->Show(m_hwnd))) {
                IShellItem *item = nullptr;
                if (SUCCEEDED(fd->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        std::wstring p = path;
                        if (_wcsicmp(pn::BaseName(p).c_str(), L"PN-A2DP") != 0) p = pn::JoinPath(p, L"PN-A2DP");
                        SetDlgItemTextW(page, IDC_FOLDER_EDIT, p.c_str());
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            fd->Release();
        }
        return TRUE;
    }
    if (page == m_page[P_DEVICES] && (id == IDC_DEV_ALL || id == IDC_DEV_NONE)) {
        HWND lv = GetDlgItem(page, IDC_DEV_LIST);
        for (int i = 0; i < ListView_GetItemCount(lv); i++) ListView_SetCheckState(lv, i, id == IDC_DEV_ALL);
        return TRUE;
    }
    return FALSE;
}

INT_PTR Wizard::OnPageNotify(HWND page, NMHDR *nm)
{
    if (page == m_page[P_COMPONENTS] && nm->idFrom == IDC_COMP_LIST && nm->code == LVN_ITEMCHANGING) {
        auto change = reinterpret_cast<NMLISTVIEW *>(nm);
        if (change->iItem >= 0 && change->iItem <= C_CLI && (change->uChanged & LVIF_STATE) &&
            (change->uNewState & LVIS_STATEIMAGEMASK) == INDEXTOSTATEIMAGEMASK(2) && !PayloadHas(m_items, kComps[change->iItem].comp)) {
            SetWindowLongPtrW(page, DWLP_MSGRESULT, TRUE); return TRUE;
        }
    }
    if (page == m_page[P_COMPONENTS] && nm->idFrom == IDC_COMP_LIST && nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && lv->iItem >= 0 && lv->iItem < COMP_COUNT) SetDlgItemTextW(page, IDC_COMP_DESC, ComponentDescription(lv->iItem).c_str());
        if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
            /* plugins need the host; the host needs the driver to be useful */
            HWND h = nm->hwndFrom;
            if ((lv->iItem == C_LDAC || lv->iItem == C_APTX || lv->iItem == C_AAC) && ListView_GetCheckState(h, lv->iItem)) ListView_SetCheckState(h, C_HOST, TRUE);
            ULONGLONG bytes = 0;
            for (auto &it : m_items) { bool sel = it.comp == L"core"; for (int i = 0; i < COMP_COUNT; i++) if (ListView_GetCheckState(h, i) && it.comp == kComps[i].comp) sel = true; if (sel) bytes += it.size; }
            wchar_t b[128]; swprintf_s(b, L"Disk space required: %llu KB", bytes / 1024 + 1);
            SetDlgItemTextW(page, IDC_COMP_SIZE, b);
        }
        return TRUE;
    }
    return FALSE;
}

int Wizard::NextOf(int p)
{
    if (m_mode == M_UNINSTALL || m_mode == M_ELEVATED_UNINSTALL) return p == P_UNINSTALL ? P_PROGRESS : P_FINISH;
    switch (p) {
    case P_WELCOME: return P_LICENSE;
    case P_LICENSE: return P_COMPONENTS;
    case P_COMPONENTS: return P_FOLDER;
    case P_FOLDER: return m_a.comps[C_DRIVER] ? P_DEVICES : P_PROGRESS;
    case P_DEVICES: return P_SIGNING;
    case P_SIGNING: return P_PROGRESS;
    case P_PROGRESS: return P_FINISH;
    }
    return P_FINISH;
}

int Wizard::PrevOf(int p)
{
    switch (p) {
    case P_LICENSE: return P_WELCOME;
    case P_COMPONENTS: return P_LICENSE;
    case P_FOLDER: return P_COMPONENTS;
    case P_DEVICES: return P_FOLDER;
    case P_SIGNING: return P_DEVICES;
    }
    return p;
}

bool Wizard::Leave(int p)
{
    if (p == P_LICENSE && IsDlgButtonChecked(m_page[P_LICENSE], IDC_LICENSE_ACCEPT) != BST_CHECKED) return false;
    if (p == P_COMPONENTS) {
        HWND lv = GetDlgItem(m_page[P_COMPONENTS], IDC_COMP_LIST);
        for (int i = 0; i < COMP_COUNT; i++) m_a.comps[i] = ListView_GetCheckState(lv, i) != 0 && (i > C_CLI || PayloadHas(m_items, kComps[i].comp));
        if (m_a.comps[C_DRIVER]) {
            std::wstring reason;
            if (CheckDriverPolicy(m_trust, m_a.allowTestDriver, reason) != ERROR_SUCCESS) {
                MessageBoxW(m_hwnd, reason.c_str(), L"Microsoft-signed driver required", MB_ICONERROR); return false;
            }
        }
        bool any = false; for (int i = 0; i < COMP_COUNT; i++) any |= m_a.comps[i];
        if (!any) { MessageBoxW(m_hwnd, L"Select at least one component.", L"PN-A2DP Setup", MB_ICONWARNING); return false; }
    }
    if (p == P_FOLDER) {
        wchar_t b[MAX_PATH] = {};
        GetDlgItemTextW(m_page[P_FOLDER], IDC_FOLDER_EDIT, b, MAX_PATH);
        std::wstring dir = b;
        if (!NormalizeInstallDir(dir)) { MessageBoxW(m_hwnd, L"Enter a full folder path below a drive root, such as C:\\Program Files\\PN-A2DP. A drive root itself cannot be used.", L"PN-A2DP Setup", MB_ICONWARNING); return false; }
        /* an existing folder that already holds other files gets its own sub-folder, as Browse does */
        if (pn::DirExists(dir) && _wcsicmp(pn::BaseName(dir).c_str(), L"PN-A2DP") != 0 && !pn::FileExists(pn::JoinPath(dir, L"uninstall.exe"))) {
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileW(pn::JoinPath(dir, L"*").c_str(), &fd);
            bool nonEmpty = false;
            if (h != INVALID_HANDLE_VALUE) { do { if (wcscmp(fd.cFileName, L".") && wcscmp(fd.cFileName, L"..")) { nonEmpty = true; break; } } while (FindNextFileW(h, &fd)); FindClose(h); }
            if (nonEmpty) dir = pn::JoinPath(dir, L"PN-A2DP");
        }
        m_a.folder = dir;
        SetDlgItemTextW(m_page[P_FOLDER], IDC_FOLDER_EDIT, dir.c_str());
    }
    if (p == P_DEVICES) {
        HWND lv = GetDlgItem(m_page[P_DEVICES], IDC_DEV_LIST);
        m_a.devices.clear();
        for (int i = 0; i < ListView_GetItemCount(lv); i++) if (ListView_GetCheckState(lv, i)) m_a.devices.push_back(m_nodes[i].instanceId);
    }
    if (p == P_SIGNING) {
        std::wstring reason;
        if (CheckDriverPolicy(m_trust, m_a.allowTestDriver, reason) != ERROR_SUCCESS) {
            MessageBoxW(m_hwnd, reason.c_str(), L"Microsoft-signed driver required", MB_ICONERROR); return false;
        }
    }
    return true;
}

void Wizard::Show(int p)
{
    m_cur = p;
    for (int i = 0; i < P_COUNT; i++) ShowWindow(m_page[i], i == p ? SW_SHOW : SW_HIDE);
    Enter(p);
}

void Wizard::Enter(int p)
{
    bool uninstall = m_mode == M_UNINSTALL || m_mode == M_ELEVATED_UNINSTALL;
    EnableWindow(GetDlgItem(m_hwnd, IDC_WZ_BACK), p != P_WELCOME && p != P_PROGRESS && p != P_FINISH && p != P_UNINSTALL);
    EnableWindow(GetDlgItem(m_hwnd, IDC_WZ_NEXT), p != P_PROGRESS);
    EnableWindow(GetDlgItem(m_hwnd, IDCANCEL), p != P_PROGRESS);
    SetDlgItemTextW(m_hwnd, IDC_WZ_NEXT, p == P_FINISH ? L"Finish" : (p == P_SIGNING || p == P_UNINSTALL || (p == P_FOLDER && !m_a.comps[C_DRIVER])) ? L"Install" : L"Next >");
    if (p == P_FINISH) SetDlgItemTextW(m_hwnd, IDC_WZ_NEXT, L"Finish");
    if (p == P_UNINSTALL) SetDlgItemTextW(m_hwnd, IDC_WZ_NEXT, L"Uninstall");
    Button_SetElevationRequiredState(GetDlgItem(m_hwnd, IDC_WZ_NEXT), !pn::IsElevated() && (p == P_SIGNING || p == P_UNINSTALL || (p == P_FOLDER && !m_a.comps[C_DRIVER])));
    switch (p) {
    case P_WELCOME: Header(L"Welcome to PN-A2DP Setup", PayloadHas(m_items, L"driver") ? L"Open Bluetooth A2DP driver with selectable codecs" : L"Windows mode - Bluetooth audio controls"); break;
    case P_LICENSE: Header(L"License", L"PN-A2DP is MIT licensed; the codec plugins carry their own licenses"); EnableWindow(GetDlgItem(m_hwnd, IDC_WZ_NEXT), IsDlgButtonChecked(m_page[P_LICENSE], IDC_LICENSE_ACCEPT) == BST_CHECKED); break;
    case P_COMPONENTS: Header(L"Components", L"Choose what to install"); break;
    case P_FOLDER: Header(L"Installation folder", L"Where the service, plugins and tools are placed"); break;
    case P_DEVICES: Header(L"Devices", L"Choose which headsets use the PN-A2DP driver"); break;
    case P_SIGNING: Header(L"Driver verification", L"Verify the package before installing"); break;
    case P_PROGRESS: Header(uninstall ? L"Removing PN-A2DP" : L"Installing", uninstall ? L"Restoring the Microsoft driver and removing files" : m_a.comps[C_DRIVER] ? L"Copying files, staging the driver, registering the service" : L"Installing Windows audio controls and shortcuts"); break;
    case P_FINISH: Header(m_result == ERROR_SUCCESS ? (uninstall ? L"Removal complete" : L"Installation complete") : (uninstall ? L"Removal finished with errors" : L"Installation finished with errors"), L""); break;
    case P_UNINSTALL: {
        Header(L"Uninstall PN-A2DP", L"Restore the Microsoft driver on every headset and remove all files");
        auto nodes = pn::EnumerateA2dpSinkNodes();
        std::wstring t = L"This removes PN-A2DP from " + pn::InstallDir() + L".\r\n\r\n";
        int n = 0;
        for (auto &d : nodes) if (d.usesPnA2dp()) { t += L"  \x2022  " + d.name + L" goes back to the Microsoft Bluetooth A2DP driver\r\n"; n++; }
        if (!n) t += L"No headset currently uses the PN-A2DP driver.\r\n";
        t += L"\r\nThe driver package is removed from the driver store, the codec host service is unregistered and the shortcuts and the Apps & features entry are deleted. Per-headset settings stored by the driver remain in the registry (HKLM\\SYSTEM\\CurrentControlSet\\Services\\pna2dp\\Parameters) until Windows cleans them up.\r\n\r\nClick Uninstall to continue.";
        SetDlgItemTextW(m_page[P_UNINSTALL], IDC_UNINST_TEXT, t.c_str());
        break;
    }
    }
}

void Wizard::StartWork()
{
    m_running = true;
    m_log.hwnd = m_hwnd;
    bool uninstall = m_mode == M_UNINSTALL || m_mode == M_ELEVATED_UNINSTALL;
    m_worker = std::thread([this, uninstall]() {
        bool reboot = false;
        DWORD err = uninstall ? DoUninstall(m_log, reboot) : DoInstall(m_a, m_items, m_log, reboot);
        PostMessageW(m_hwnd, WM_APP_DONE, err, reboot ? 1 : 0);
    });
}

void Wizard::OnDone(DWORD err, bool reboot)
{
    if (m_worker.joinable()) m_worker.join();
    m_running = false;
    m_result = err;
    m_reboot = reboot;
    bool uninstall = m_mode == M_UNINSTALL || m_mode == M_ELEVATED_UNINSTALL;
    /* keep the full log on disk for support; the Finish page shows it in a scrollable box */
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring logPath = pn::JoinPath(tmp, uninstall ? L"pna2dp-uninstall.log" : L"pna2dp-setup.log");
    WriteUtf8File(logPath, m_log.text);
    std::wstring t;
    if (err == ERROR_SUCCESS) {
        t = uninstall ? L"PN-A2DP has been removed." : m_a.comps[C_DRIVER] ? L"PN-A2DP is installed." : L"PN-A2DP Windows audio controls are installed. Devices using the Microsoft driver open in Windows mode. Use the Driver tab to switch an existing PN-A2DP device to Windows mode.";
        if (!uninstall) t += L"\r\n\r\nUse the control panel to pick codecs, tune them and watch the live status. The command line tool pna2dpctl.exe lives in the installation folder. Each headset keeps its own settings.";
        if (!uninstall && m_a.comps[C_DRIVER] && m_a.devices.empty()) t += L"\r\n\r\nNo headset was switched to PN-A2DP yet; do that on the Driver tab of the control panel.";
    } else {
        t = (uninstall ? L"Removal finished with errors: " : L"Installation finished with errors: ") + pn::Win32ErrorText(err) + L"\r\n\r\nLog (also saved as " + logPath + L"):\r\n\r\n" + m_log.text;
    }
    if (reboot) t += L"\r\n\r\nWindows needs a restart to complete the driver change.";
    SetDlgItemTextW(m_page[P_FINISH], IDC_FINISH_TEXT, t.c_str());
    bool canLaunch = !uninstall && m_a.comps[C_PANEL] && pn::FileExists(pn::JoinPath(m_a.folder, L"pna2dp-panel.exe"));
    EnableWindow(GetDlgItem(m_page[P_FINISH], IDC_FINISH_LAUNCH), canLaunch && !reboot);
    CheckDlgButton(m_page[P_FINISH], IDC_FINISH_LAUNCH, canLaunch && !reboot && err == ERROR_SUCCESS ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(m_page[P_FINISH], IDC_FINISH_REBOOT), reboot);
    CheckDlgButton(m_page[P_FINISH], IDC_FINISH_REBOOT, BST_UNCHECKED);
    Show(P_FINISH);
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                  */
/* ------------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    /* the copy that lives in the installation folder is the uninstaller: no arguments means uninstall */
    bool uninstall = _wcsicmp(pn::BaseName(pn::ModulePath()).c_str(), L"uninstall.exe") == 0;
    bool silent = false, elevated = false, noDevices = false, check = false, allowTest = false;
    std::wstring answersFile, extractDir, dir, verifyFolder, reportPath;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--uninstall" || a == L"/uninstall") uninstall = true;
        else if (a == L"--install") uninstall = false;
        else if (a == L"--silent" || a == L"/S" || a == L"/silent" || a == L"/quiet" || a == L"/q") silent = true;
        else if (a == L"--elevated") elevated = true;
        else if (a == L"--no-devices") noDevices = true;
        else if (a == L"--allow-test-driver") allowTest = true;
        else if (a == L"--check") check = true;
        else if (a == L"--verify-package" && i + 1 < argc) { check = true; verifyFolder = argv[++i]; }
        else if (a == L"--report" && i + 1 < argc) reportPath = argv[++i];
        else if (a == L"--elevated-install" && i + 1 < argc) { elevated = true; answersFile = argv[++i]; }
        else if (a == L"--extract" && i + 1 < argc) extractDir = argv[++i];
        else if (a == L"--dir" && i + 1 < argc) dir = argv[++i];
        else { LocalFree(argv); return ERROR_INVALID_PARAMETER; }
    }
    LocalFree(argv);
    if (!reportPath.empty() && !check) return ERROR_INVALID_PARAMETER;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS };
    InitCommonControlsEx(&icc);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    std::vector<PayloadItem> items = ReadPayloadIndex();
    if (!uninstall && items.empty()) { MessageBoxW(NULL, L"This installer carries no payload (built without -Target setup after the other targets).", L"PN-A2DP Setup", MB_ICONERROR); return ERROR_RESOURCE_NAME_NOT_FOUND; }

    if (!extractDir.empty()) {
        pn::CreateDirectoryRecursive(extractDir);
        if (PayloadHas(items, L"driver")) pn::CreateDirectoryRecursive(pn::JoinPath(extractDir, L"driver"));
        DWORD err = ERROR_SUCCESS;
        for (auto &it : items) { DWORD e = ExtractItem(it, pn::JoinPath(it.sub.empty() ? extractDir : pn::JoinPath(extractDir, it.sub), it.file)); if (e != ERROR_SUCCESS) err = e; }
        return (int)err;
    }

    Answers a;
    a.allowTestDriver = allowTest;
    if (!dir.empty() && !NormalizeInstallDir(dir)) { if (!silent) MessageBoxW(NULL, L"--dir must be a full folder path below a drive root.", L"PN-A2DP Setup", MB_ICONERROR); return ERROR_INVALID_NAME; }
    a.folder = dir.empty() ? pn::InstallDir() : dir;
    if (!answersFile.empty()) { if (!a.Load(answersFile) || !NormalizeInstallDir(a.folder)) { MessageBoxW(NULL, L"The installation answers could not be read or have an invalid folder.", L"PN-A2DP Setup", MB_ICONERROR); return ERROR_INVALID_DATA; } DeleteFileW(answersFile.c_str()); }
    for (int i = 0; i < COMP_COUNT; i++) if (i <= C_CLI && !PayloadHas(items, kComps[i].comp)) a.comps[i] = false;

    if (check && verifyFolder.empty() && !PayloadHas(items, L"driver")) {
        Log log;
        DWORD err = ERROR_SUCCESS;
        if (!PayloadHas(items, L"panel") || !PayloadHas(items, L"cli")) err = ERROR_RESOURCE_NAME_NOT_FOUND;
        for (const auto &item : items) {
            const void *data = nullptr; DWORD bytes = 0;
            if (!ReadResource(item.res.c_str(), &data, &bytes) || !bytes || bytes != item.size) err = ERROR_INVALID_DATA;
            if (item.comp != L"panel" && item.comp != L"cli" && item.comp != L"core") err = ERROR_INVALID_DATA;
        }
        log(L"Windows mode package: no PN-A2DP kernel driver, codec host or codec plugins.");
        log(L"Driver-signing policy does not apply to these application files. Boot policy is unchanged.");
        log(L"Result: 0x%08lX", err);
        if (reportPath.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); reportPath = pn::JoinPath(tmp, L"pna2dp-preflight.log"); }
        if (!WriteUtf8File(reportPath, log.text)) return ERROR_WRITE_FAULT;
        return (int)err;
    }
    if (check) {
        auto trust = verifyFolder.empty() ? InspectPayloadTrust(items) : pn::VerifyDriverPackage(verifyFolder);
        std::wstring reason;
        DWORD err = CheckDriverPolicy(trust, a.allowTestDriver, reason);
        Log log;
        log(L"Catalog trusted: %s", trust.trusted ? L"yes" : L"no");
        log(L"Microsoft release driver: %s", trust.microsoftDriver ? L"yes" : L"no");
        log(L"Attestation EKU: %s", trust.attestation ? L"yes" : L"no");
        log(L"Signer: %s", trust.signer.c_str());
        log(L"Result: 0x%08lX", err);
        log(L"%s", reason.c_str());
        if (reportPath.empty()) { wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp); reportPath = pn::JoinPath(tmp, L"pna2dp-preflight.log"); }
        if (!WriteUtf8File(reportPath, log.text)) return ERROR_WRITE_FAULT;
        return (int)err;
    }

    if (silent) {
        // Reject unusable packages before UAC, folder creation or service changes.
        if (!uninstall) {
            Log preflight;
            DWORD err = CheckPayload(a, items, preflight);
            if (err) {
                wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
                WriteUtf8File(pn::JoinPath(tmp, L"pna2dp-setup.log"), preflight.text);
                return (int)err;
            }
        }
        if (!pn::IsElevated()) {
            std::wstring params = uninstall ? L"--uninstall --silent --elevated" : (L"--silent --elevated" + std::wstring(noDevices ? L" --no-devices" : L"") + std::wstring(a.allowTestDriver ? L" --allow-test-driver" : L"") + (dir.empty() ? L"" : L" --dir \"" + dir + L"\""));
            DWORD code = 0;
            DWORD err = pn::RunElevated(pn::ModulePath(), params, code);
            return err == ERROR_SUCCESS ? (int)code : (int)err;
        }
        Log log;
        bool reboot = false;
        DWORD err;
        if (uninstall) err = DoUninstall(log, reboot);
        else {
            if (!noDevices && a.comps[C_DRIVER]) for (auto &n : pn::EnumerateA2dpSinkNodes()) a.devices.push_back(n.instanceId);
            err = DoInstall(a, items, log, reboot);
        }
        /* leave a log next to the installation for unattended runs */
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        std::wstring logPath = pn::JoinPath(uninstall ? std::wstring(tmp) : a.folder, uninstall ? L"pna2dp-uninstall.log" : L"pna2dp-setup.log");
        WriteUtf8File(logPath, log.text);
        if (err == ERROR_SUCCESS && reboot) err = ERROR_SUCCESS_REBOOT_REQUIRED;
        ScheduleSelfDelete();
        return (int)err;
    }

    Mode mode = uninstall ? (elevated ? M_ELEVATED_UNINSTALL : M_UNINSTALL) : (elevated ? M_ELEVATED_INSTALL : M_INSTALL);
    if (elevated && !pn::IsElevated()) { MessageBoxW(NULL, L"Setup needs administrator rights.", L"PN-A2DP Setup", MB_ICONERROR); return ERROR_ELEVATION_REQUIRED; }
    Wizard w(mode, items, a);
    int rc = w.Run();
    CoUninitialize();
    ScheduleSelfDelete();
    return rc;
}
