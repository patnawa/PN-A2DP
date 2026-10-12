/* Protected executable locations for privileged installation and codec loading.
 * Rejects ambiguous/redirected paths and non-administrator modification rights.
 * This is a conservative allow policy, not an effective-access calculator.
 * SPDX-License-Identifier: MIT */
#pragma once
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <string>
#include <algorithm>

namespace pn {
namespace securepath {
inline bool TrustedSid(PSID sid) {
    if (!sid || !IsValidSid(sid)) return false;
    if (IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)) return true;
    struct InstallerSid {
        PSID value = nullptr;
        InstallerSid() { ConvertStringSidToSidW(L"S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464", &value); }
        ~InstallerSid() { if (value) LocalFree(value); }
    };
    static InstallerSid installer;
    return installer.value && EqualSid(sid, installer.value);
}
inline bool ProtectedDescriptor(PSECURITY_DESCRIPTOR sd, bool contents) {
    PSID owner = nullptr; BOOL defaulted = FALSE, present = FALSE; PACL dacl = nullptr;
    if (!sd || !IsValidSecurityDescriptor(sd) || !GetSecurityDescriptorOwner(sd, &owner, &defaulted) ||
        !TrustedSid(owner) || !GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) return false;
    DWORD forbidden = DELETE | WRITE_DAC | WRITE_OWNER | FILE_DELETE_CHILD;
    if (contents) forbidden |= FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
    for (DWORD i = 0; i < dacl->AceCount; ++i) {
        void *raw = nullptr;
        if (!GetAce(dacl, i, &raw)) return false;
        auto header = (ACE_HEADER *)raw;
        if (header->AceFlags & INHERIT_ONLY_ACE) continue;
        if (header->AceType == ACCESS_DENIED_ACE_TYPE) continue;
        // Unknown/object/conditional grants are rejected rather than guessed.
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || header->AceSize < sizeof(ACCESS_ALLOWED_ACE)) return false;
        auto ace = (ACCESS_ALLOWED_ACE *)raw;
        DWORD mask = ace->Mask; MapGenericMask(&mask, &mapping);
        if ((mask & forbidden) && !TrustedSid((PSID)&ace->SidStart)) return false;
    }
    return true;
}
inline DWORD Object(const std::wstring &path, bool contents, bool *directory = nullptr) {
    HANDLE h = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    BY_HANDLE_FILE_INFORMATION info{};
    DWORD error = ERROR_SUCCESS;
    if (!GetFileInformationByHandle(h, &info)) error = GetLastError();
    else if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && info.nNumberOfLinks != 1)) error = ERROR_ACCESS_DENIED;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!error) error = GetSecurityInfo(h, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &sd);
    if (!error && !ProtectedDescriptor(sd, contents)) error = ERROR_ACCESS_DENIED;
    if (!error && directory) *directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (sd) LocalFree(sd);
    CloseHandle(h);
    return error;
}
inline DWORD Canonical(const std::wstring &input, std::wstring &path) {
    path = input; std::replace(path.begin(), path.end(), L'/', L'\\');
    if (path.size() <= 3 || path.size() >= MAX_PATH || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z'))) return ERROR_ACCESS_DENIED;
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    size_t start = 3;
    while (start < path.size()) {
        size_t end = path.find(L'\\', start); if (end == std::wstring::npos) end = path.size();
        auto part = path.substr(start, end-start);
        if (part.empty() || part.back() == L'.' || part.back() == L' ' || part.find_first_of(L":*?\"<>|") != std::wstring::npos)
            return ERROR_ACCESS_DENIED;
        start = end + 1;
    }
    return GetDriveTypeW(path.substr(0,3).c_str()) == DRIVE_FIXED ? ERROR_SUCCESS : ERROR_ACCESS_DENIED;
}
inline DWORD Path(const std::wstring &input, bool directory, bool allowMissing) {
    std::wstring path;
    DWORD error = Canonical(input, path); if (error) return error;
    std::wstring parent = path.substr(0,3);
    error = Object(parent, false); if (error) return error;
    size_t end = 3;
    while (end < path.size()) {
        end = path.find(L'\\', end); if (end == std::wstring::npos) end = path.size();
        std::wstring current = path.substr(0,end);
        bool isDir = false;
        error = Object(current, end == path.size(), &isDir);
        if (allowMissing && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND))
            return Object(parent, true); // the nearest existing parent must prevent precreation
        if (error) return error;
        if ((end < path.size() || directory) != isDir) return ERROR_ACCESS_DENIED;
        parent = current; ++end;
    }
    return ERROR_SUCCESS;
}
inline DWORD Tree(const std::wstring &path, unsigned depth, unsigned &count) {
    if (depth > 24 || ++count > 4096) return ERROR_ACCESS_DENIED;
    bool directory = false;
    DWORD error = Object(path, true, &directory); if (error || !directory) return error;
    WIN32_FIND_DATAW entry{};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) { error = GetLastError(); return error == ERROR_FILE_NOT_FOUND ? ERROR_SUCCESS : error; }
    do {
        if (!wcscmp(entry.cFileName,L".") || !wcscmp(entry.cFileName,L"..")) continue;
        error = Tree(path + L"\\" + entry.cFileName, depth+1, count);
        if (error) break;
    } while (FindNextFileW(search, &entry));
    if (!error && GetLastError() != ERROR_NO_MORE_FILES) error = GetLastError();
    FindClose(search);
    return error;
}
}
// No ACLs are silently rewritten. The user can choose a protected location.
inline DWORD ValidateProtectedDirectory(const std::wstring &path, bool allowMissing = false) {
    return securepath::Path(path, true, allowMissing);
}
inline DWORD ValidateProtectedService(const std::wstring &exe) {
    DWORD error = securepath::Path(exe, false, false); if (error) return error;
    auto directory = exe.substr(0, exe.find_last_of(L"\\/"));
    error = ValidateProtectedDirectory(directory); if (error) return error;
    unsigned count = 0;
    return securepath::Tree(directory, 0, count); // includes dependent DLLs and plugins
}
}
