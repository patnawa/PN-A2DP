/* Read-only audio diagnostics and bounded transport history. SPDX-License-Identifier: MIT */
#pragma once
#include "pnctl.h"
#include "pndrv.h"
#include "pnaudio.h"

namespace pn {
enum class HealthLevel { Pass, Info, Warning, Error, Unknown };
enum class HealthAction { None, Sound, Bluetooth, Driver, Adapter, Codecs };
struct HealthCheck {
    HealthLevel level;
    std::wstring name, detail;
    HealthAction action = HealthAction::None;
};
struct HealthSnapshot {
    bool demo = false, nodeKnown = false, found = false;
    DWORD nodeError = 0, controlError = 0;
    A2dpNode node;
    WindowsAudioInfo audio;
    SigningState signing;
    bool serviceKnown = false;
    DWORD serviceState = 0;
    bool statusValid = false, statsValid = false, versionValid = false;
    bool capsValid = false, configValid = false, qualityValid = false;
    PNA2DP_STATUS status{};
    PNA2DP_STATS stats{};
    PNA2DP_VERSION version{};
    PNA2DP_CAPABILITIES caps{};
    PNA2DP_CONFIG config{};
    PNA2DP_QUALITY quality{};
    SYSTEMTIME capturedUtc{};
};
struct QualitySample {
    ULONGLONG tick = 0;
    PNA2DP_STATUS status{};
    PNA2DP_STATS stats{};
    bool qualityValid = false;
    PNA2DP_QUALITY quality{};
};
struct QualitySummary {
    HealthLevel level = HealthLevel::Unknown;
    std::wstring title, reason, recent;
    ULONG seconds = 0;
    ULONGLONG packets = 0, drops = 0, failures = 0, trims = 0;
};
class QualityHistory {
    std::vector<QualitySample> samples;
public:
    void Reset() { samples.clear(); }
    void Observe(ULONGLONG tick, bool valid, const PNA2DP_STATUS &status, const PNA2DP_STATS &stats,
                 bool qualityValid, const PNA2DP_QUALITY &quality);
    QualitySummary Summary(ULONGLONG now) const;
    const std::vector<QualitySample> &Samples() const { return samples; }
};
// Worker-safe: owns a separate read-only control handle; does not subscribe to events.
HealthSnapshot CollectHealth(const std::wstring &instanceId);
std::vector<HealthCheck> EvaluateHealth(const HealthSnapshot &snapshot, const QualitySummary &recent);
const wchar_t *HealthLevelName(HealthLevel level);
std::wstring HealthCheckText(const std::vector<HealthCheck> &checks);
// Allowlisted fields only: no names, addresses, device/endpoint IDs, paths or raw trace text.
std::wstring DiagnosticReport(const HealthSnapshot &snapshot, const QualityHistory &history, ULONGLONG now,
                              const std::wstring &applicationVersion);
}
