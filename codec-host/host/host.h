/*
 * host.h - PN-A2DP codec host internals.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <thread>
#include <mutex>
#include "pna2dp_public.h"
#include "pna2dp_codec_plugin.h"

void HostLog(const char *fmt, ...);

/* ---- plugins ---------------------------------------------------------------- */
struct LoadedPlugin {
    HMODULE module = NULL;            /* NULL for built-in */
    const PnCodecPlugin *api = nullptr;
    std::wstring path;
};

class PluginRegistry {
public:
    void AddBuiltin(const PnCodecPlugin *api);
    void LoadDirectory(const std::wstring &dir);
    uint32_t CodecMask() const;
    const PnCodecPlugin *ForCodec(uint32_t codec) const;
    const std::vector<LoadedPlugin> &All() const { return m_plugins; }
private:
    std::vector<LoadedPlugin> m_plugins;
};

/* ---- one driver device instance ------------------------------------------- */
class DeviceWorker {
public:
    DeviceWorker(const std::wstring &path, PluginRegistry &plugins);
    ~DeviceWorker();
    bool Start();
    void Stop();
    const std::wstring &Path() const { return m_path; }
    bool Alive() const { return m_alive; }
private:
    void Run();
    void HandleWork(const PNA2DP_CODEC_WORK *w, const uint8_t *pcm);
    void Open(const PNA2DP_CODEC_WORK *w);
    void Encode(const PNA2DP_CODEC_WORK *w, const uint8_t *pcm);
    void CloseEncoder();
    void PutResult(uint32_t seq, uint32_t gen, int status, const PnCodecOutput *out);

    std::wstring m_path;
    PluginRegistry &m_plugins;
    HANDLE m_device = INVALID_HANDLE_VALUE;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_alive{false};

    const PnCodecPlugin *m_plugin = nullptr;
    PnCodecEncoder *m_encoder = nullptr;
    PNA2DP_STREAM_PARAMS m_params{};
    uint32_t m_generation = 0;
    std::vector<uint8_t> m_outBuf;
    std::vector<uint8_t> m_resultBuf;
    uint32_t m_lastQuality = 0;
    uint32_t m_lastBitrate = 0;
};

/* built-in reference plugin (SBC via common/sbc) */
extern "C" const PnCodecPlugin *PnBuiltinSbcPlugin(void);
