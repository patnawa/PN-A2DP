/*
 * plugins.cpp - codec plugin discovery and loading.
 * SPDX-License-Identifier: MIT
 */
#include "host.h"
#include <stdio.h>
#include <stdarg.h>

void HostLog(const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    printf("[%02u:%02u:%02u.%03u] %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, buf);
    fflush(stdout);
    OutputDebugStringA(buf);
}

void PluginRegistry::AddBuiltin(const PnCodecPlugin *api)
{
    if (!api || api->api_version != PNA2DP_PLUGIN_API_VERSION) return;
    LoadedPlugin p;
    p.api = api;
    p.path = L"(built-in)";
    m_plugins.push_back(p);
    HostLog("plugin %s %s (%s) built-in, codecs 0x%08x", api->name, api->version, api->license, api->codec_mask);
}

void PluginRegistry::LoadDirectory(const std::wstring &dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t full[MAX_PATH];
    DWORD length = GetFullPathNameW(dir.c_str(), MAX_PATH, full, NULL);
    if (!length || length >= MAX_PATH) return;   /* Never use a truncated DLL directory. */
    std::wstring base(full);
    std::wstring pattern = base + L"\\pna2dp-codec-*.dll";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring path = base + L"\\" + fd.cFileName;
        HMODULE m = LoadLibraryExW(path.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) { HostLog("plugin %ls: LoadLibrary failed %lu", fd.cFileName, GetLastError()); continue; }
        PnCodecPluginGetInfoFn fn = (PnCodecPluginGetInfoFn)GetProcAddress(m, PNA2DP_PLUGIN_ENTRY);
        const PnCodecPlugin *api = fn ? fn() : nullptr;
        if (!api || api->api_version != PNA2DP_PLUGIN_API_VERSION || !api->open || !api->encode || !api->close) {
            HostLog("plugin %ls: not a valid PN-A2DP codec plugin", fd.cFileName);
            FreeLibrary(m);
            continue;
        }
        LoadedPlugin p;
        p.module = m;
        p.api = api;
        p.path = path;
        m_plugins.push_back(p);
        HostLog("plugin %s %s (%s) from %ls, codecs 0x%08x", api->name, api->version, api->license, fd.cFileName, api->codec_mask);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

uint32_t PluginRegistry::CodecMask() const
{
    uint32_t m = 0;
    for (auto &p : m_plugins) m |= p.api->codec_mask;
    return m;
}

const PnCodecPlugin *PluginRegistry::ForCodec(uint32_t codec) const
{
    /* external plugins take precedence over built-ins so users can upgrade encoders */
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (it->api->codec_mask & PNA2DP_CODEC_BIT(codec)) return it->api;
    }
    return nullptr;
}
