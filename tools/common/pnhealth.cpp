/* SPDX-License-Identifier: MIT */
#include "pnhealth.h"
#include <algorithm>
#include <sstream>
#include <iomanip>

namespace pn {
static std::wstring Number(ULONGLONG n) { return std::to_wstring(n); }
static std::wstring Hex(ULONG n) {
    wchar_t value[16]{}; swprintf_s(value, L"0x%08lX", n); return value;
}
const wchar_t *HealthLevelName(HealthLevel level) {
    switch (level) {
    case HealthLevel::Pass: return L"OK";
    case HealthLevel::Info: return L"INFO";
    case HealthLevel::Warning: return L"ATTENTION";
    case HealthLevel::Error: return L"PROBLEM";
    default: return L"UNKNOWN";
    }
}
void QualityHistory::RecordEvent(ULONGLONG tick, const std::wstring &text) {
    if (!events.empty() && tick < events.back().tick) events.clear();
    events.push_back({tick, text});
    if (events.size() > 32) events.erase(events.begin());
}
std::wstring QualityHistory::EventText(ULONGLONG now) const {
    if (events.empty()) return L"No quality events observed yet.";
    std::wstring text;
    for (auto it = events.rbegin(); it != events.rend(); ++it)
        text += Number(now >= it->tick ? (now-it->tick)/1000 : 0) + L" s ago: " + it->text + L"\r\n";
    return text;
}
static bool Active(const PNA2DP_STATUS &s) { return s.State == PNA2DP_STATE_STREAMING && (s.Flags & PNA2DP_STATUS_FLAG_AUDIO_ACTIVE); }
bool QualityHistory::SuggestSbcFallback(ULONGLONG now) const {
    if (samples.size() < 6 || now < samples.back().tick || now-samples.back().tick > 2500) return false;
    const auto &last = samples.back();
    if (!Active(last.status) || (last.status.Codec != PNA2DP_CODEC_APTX &&
        last.status.Codec != PNA2DP_CODEC_APTX_HD && last.status.Codec != PNA2DP_CODEC_APTX_LL)) return false;
    size_t first = samples.size()-1;
    // Include the sample immediately before the boundary: real timers jitter,
    // so requiring an observation at exactly ten seconds suppresses the offer.
    while (first && last.tick-samples[first].tick < 10000) --first;
    if (last.tick-samples[first].tick < 10000) return false;
    if (last.stats.PacketsSent == samples[first].stats.PacketsSent) return true;
    unsigned bad = 0;
    for (size_t i = first+1; i < samples.size(); ++i) {
        const auto &a = samples[i], &b = samples[i-1];
        if ((a.qualityValid && a.quality.Health == PNA2DP_QUALITY_CONGESTED) ||
            a.stats.WriteFailures > b.stats.WriteFailures || a.stats.PacketsDropped > b.stats.PacketsDropped) ++bad;
    }
    return bad >= 5; // persistent recent trouble, not a single historical counter
}
void QualityHistory::Observe(ULONGLONG tick, bool valid, const PNA2DP_STATUS &s, const PNA2DP_STATS &t,
                             bool qv, const PNA2DP_QUALITY &q) {
    if (!valid) {
        if (!samples.empty()) RecordEvent(tick, L"Observations interrupted; connection health is unknown.");
        samples.clear(); return;
    }
    if (!samples.empty()) {
        const auto &p = samples.back();
        if (tick < p.tick) { Reset(); Observe(tick, valid, s, t, qv, q); return; }
        if (tick-p.tick >= 900) {
            if (s.Codec != p.status.Codec) RecordEvent(tick, L"Codec changed: " + std::wstring(CodecName(p.status.Codec)) + L" -> " + CodecName(s.Codec));
            else if (Active(s) && Active(p.status) && tick-p.tick <= 5000 && s.BitrateBps && p.status.BitrateBps && s.BitrateBps != p.status.BitrateBps) {
                std::wstring reason = L"cause unavailable (settings or unobserved change)";
                if (qv && p.qualityValid && (q.Flags & p.quality.Flags & PNA2DP_QUALITY_CHANGE_DETAILS) && q.ChangeSequence > p.quality.ChangeSequence) {
                    reason.clear();
                    if (q.LastChangeReason & PNA2DP_QUALITY_REASON_RECOVERY) reason += L"healthy-traffic recovery; ";
                    if (q.LastChangeReason & PNA2DP_QUALITY_REASON_SLOW) reason += L"slow transport writes; ";
                    if (q.LastChangeReason & PNA2DP_QUALITY_REASON_FAILED) reason += L"write failures; ";
                    if (q.LastChangeReason & PNA2DP_QUALITY_REASON_PRESSURE) reason += L"send queue pressure; ";
                    if (q.LastChangeReason & PNA2DP_QUALITY_REASON_BACKLOG) reason += L"audio backlog; ";
                    if (reason.empty()) reason = L"driver adaptation";
                }
                RecordEvent(tick, Number(p.status.BitrateBps/1000) + L" -> " + Number(s.BitrateBps/1000) + L" kbps: " + reason);
            }
            if (s.State != p.status.State || Active(s) != Active(p.status)) RecordEvent(tick, L"Stream state: " + std::wstring(StateName(s.State)) + (Active(s) ? L"; audio active" : L"; audio idle"));
        }
        // No deltas across resets, reconnects, codec changes or unobserved time.
        if (tick < p.tick || tick - p.tick > 5000 || s.Codec != p.status.Codec || s.State != p.status.State ||
            ((s.Flags ^ p.status.Flags) & PNA2DP_STATUS_FLAG_AUDIO_ACTIVE) ||
            t.PacketsSent < p.stats.PacketsSent || t.PacketsDropped < p.stats.PacketsDropped ||
            t.WriteFailures < p.stats.WriteFailures || t.Underruns < p.stats.Underruns ||
            t.PcmFramesConsumed < p.stats.PcmFramesConsumed) samples.clear();
        else if (tick - p.tick < 900) return;
    }
    else RecordEvent(tick, L"Observations started; " + std::wstring(CodecName(s.Codec)) + L". Counters start a new window.");
    samples.push_back({tick, s, t, qv, q});
    while (samples.size() > 61 || (!samples.empty() && tick - samples.front().tick > 60000)) samples.erase(samples.begin());
}
QualitySummary QualityHistory::Summary(ULONGLONG now) const {
    QualitySummary result;
    result.title = L"Waiting for observations";
    result.reason = L"Play audio to measure this connection. Missing data is not a healthy result.";
    result.recent = L"No recent transport measurements";
    if (samples.empty()) return result;
    const auto &last = samples.back(); const auto &first = samples.front();
    if (now < last.tick || now - last.tick > 2500) {
        result.title = L"Measurements paused"; result.reason = L"The last sample is stale. Waiting for a fresh observation."; return result;
    }
    result.seconds = (ULONG)((last.tick - first.tick) / 1000);
    result.packets = last.stats.PacketsSent - first.stats.PacketsSent;
    result.drops = last.stats.PacketsDropped - first.stats.PacketsDropped;
    result.failures = last.stats.WriteFailures - first.stats.WriteFailures;
    result.trims = last.stats.Underruns - first.stats.Underruns;
    result.recent = L"Observed " + Number(result.seconds) + L" s: " + Number(result.packets) + L" packets, " + Number(result.drops) +
        L" dropped, " + Number(result.failures) + L" write failures, " + Number(result.trims) + L" latency trims";
    if (last.status.State != PNA2DP_STATE_STREAMING || !(last.status.Flags & PNA2DP_STATUS_FLAG_AUDIO_ACTIVE)) {
        result.level = HealthLevel::Info; result.title = L"Ready / idle";
        result.reason = L"Audio is not running. Idle time cannot establish connection quality."; return result;
    }
    if (result.drops || result.failures || result.trims) {
        result.level = HealthLevel::Warning; result.title = L"Recent transport errors";
        result.reason = L"Errors occurred in this observation window. Check distance and interference; open Adapter for transport details.";
    } else if (result.seconds >= 3 && !result.packets) {
        result.level = HealthLevel::Warning; result.title = L"No packet progress";
        result.reason = L"The stream reports active but no packets advanced. Check the playing app and the receiver connection.";
    } else if (last.qualityValid && last.quality.Health == PNA2DP_QUALITY_CONGESTED) {
        result.level = HealthLevel::Warning; result.title = L"Connection congested";
        result.reason = (last.quality.Flags & PNA2DP_QUALITY_ABR) ? L"Sustained congestion can lower bitrate within your configured limits." :
            L"Adaptive bitrate is off. Persistent congestion may require lower quality settings.";
    } else if (last.qualityValid && last.quality.Health == PNA2DP_QUALITY_RECOVERING) {
        result.level = HealthLevel::Info; result.title = L"Quality recovering";
        ULONG target = (last.quality.Flags & PNA2DP_QUALITY_CHANGE_DETAILS) && last.quality.RecoveryTargetMs >= 5000 && last.quality.RecoveryTargetMs <= 60000 ? last.quality.RecoveryTargetMs : 30000;
        result.reason = (last.quality.Flags & PNA2DP_QUALITY_ABR) ?
            L"Reduced bitrate; " + Number((target - (std::min)(last.quality.HealthyMs, target) + 999) / 1000) + L" s of healthy traffic still required before the next upward step." :
            L"Bitrate is reduced. Adaptive bitrate is currently off.";
    } else if (result.seconds >= 3 && result.packets) {
        result.level = HealthLevel::Pass; result.title = L"Transport healthy";
        result.reason = L"Packets are advancing without observed drops or write failures. This does not measure acoustic quality.";
    } else { result.level = HealthLevel::Info; result.title = L"Measuring playback"; }
    if (last.qualityValid && (last.quality.Flags & PNA2DP_QUALITY_CONSERVATIVE)) result.reason += L" A Basic Rate adapter limit is applied.";
    if (last.status.Codec == PNA2DP_CODEC_APTX || last.status.Codec == PNA2DP_CODEC_APTX_HD || last.status.Codec == PNA2DP_CODEC_APTX_LL)
        result.reason += L" This codec has a fixed bitrate; recovery may require another codec.";
    return result;
}
HealthSnapshot CollectHealth(const std::wstring &id) {
    HealthSnapshot h;
    auto nodes = EnumerateA2dpSinkNodes(&h.nodeError);
    h.nodeKnown = h.nodeError == ERROR_SUCCESS;
    for (const auto &node : nodes) if (!_wcsicmp(id.c_str(), node.instanceId.c_str())) { h.node = node; h.found = true; break; }
    if (h.found) h.audio = QueryWindowsAudio(id);
    h.signing = QuerySigningState();
    ServiceInfo service;
    h.serviceKnown = QueryService(PN_HOST_SERVICE, service) && service.state != 0;
    h.serviceState = service.state;
    if (h.found && h.node.usesPnA2dp()) {
        h.controlError = ERROR_NOT_FOUND;
        for (const auto &iface : EnumerateControlInterfaces()) if (!_wcsicmp(id.c_str(), iface.instanceId.c_str())) {
            Device device;
            if (!device.Open(iface.path, false)) { h.controlError = device.LastError(); break; }
            h.statusValid = device.GetStatus(h.status) && h.status.Size == sizeof(h.status);
            h.controlError = h.statusValid ? ERROR_SUCCESS : device.LastError() ? device.LastError() : ERROR_INVALID_DATA;
            h.statsValid = device.GetStats(h.stats) && h.stats.Size == sizeof(h.stats);
            h.versionValid = device.GetVersion(h.version) && h.version.AbiVersion == PNA2DP_ABI_VERSION;
            h.capsValid = device.GetCapabilities(h.caps) && h.caps.Size == sizeof(h.caps) && h.caps.SepCount <= PNA2DP_MAX_REMOTE_SEPS;
            h.configValid = device.GetConfig(h.config) && h.config.Size == sizeof(h.config) && h.config.Version == PNA2DP_ABI_VERSION;
            h.qualityValid = device.GetQuality(h.quality);
            break;
        }
    }
    GetSystemTime(&h.capturedUtc);
    return h;
}
std::vector<HealthCheck> EvaluateHealth(const HealthSnapshot &h, const QualitySummary &recent) {
    std::vector<HealthCheck> checks;
    auto add = [&](HealthLevel l, const wchar_t *name, std::wstring detail, HealthAction a = HealthAction::None) { checks.push_back({l, name, detail, a}); };
    if (h.demo) add(HealthLevel::Info, L"Demo", L"Simulated data. No physical audio check was performed.");
    if (!h.nodeKnown) { add(HealthLevel::Unknown, L"Device", L"Device enumeration failed: " + Hex(h.nodeError), HealthAction::Bluetooth); return checks; }
    if (!h.found || !h.node.present) { add(HealthLevel::Error, L"Device", L"Selected Bluetooth device is unavailable. Power it on and check Bluetooth settings.", HealthAction::Bluetooth); return checks; }
    add(h.node.problemCode ? HealthLevel::Error : HealthLevel::Pass, L"Device", h.node.problemCode ? L"Windows device problem code " + Number(h.node.problemCode) + L". Inspect Driver requirements." : L"Device is present; no Windows device problem reported.", h.node.problemCode ? HealthAction::Driver : HealthAction::None);
    if (h.node.restartRequired) add(HealthLevel::Warning, L"Driver change", L"Windows reports that a restart is required to finish the driver change.", HealthAction::Driver);
    if (!h.audio.active) add(FAILED(h.audio.error) ? HealthLevel::Unknown : HealthLevel::Error, L"Audio output", L"No active output was resolved for this exact device. Check connection and Sound settings. Query: " + Hex(h.audio.error), HealthAction::Sound);
    else {
        add(HealthLevel::Pass, L"Audio output", L"An active render endpoint belongs to this device.");
        add(!h.audio.defaultKnown ? HealthLevel::Unknown : h.audio.isDefault ? HealthLevel::Pass : HealthLevel::Info, L"Output selection",
            !h.audio.defaultKnown ? L"Default output could not be queried." : h.audio.isDefault ? L"Selected as the Windows multimedia default." : L"Not the default output. Choose it in your app or Sound settings.", HealthAction::Sound);
        if (FAILED(h.audio.muteError) || FAILED(h.audio.volumeError)) add(HealthLevel::Unknown, L"Volume", L"Volume or mute could not be read.", HealthAction::Sound);
        else add(h.audio.muted || h.audio.volume <= 0 ? HealthLevel::Warning : HealthLevel::Pass, L"Volume",
            L"Windows volume " + Number((ULONGLONG)(h.audio.volume * 100 + .5f)) + L"%; " + (h.audio.muted ? L"muted. Unmute in Sound settings." : L"not muted. Receiver hardware volume may be separate."), HealthAction::Sound);
        add(SUCCEEDED(h.audio.formatError) ? HealthLevel::Pass : HealthLevel::Unknown, L"Windows mix", SUCCEEDED(h.audio.formatError) ?
            Number(h.audio.sampleRate) + L" Hz, " + Number(h.audio.channels) + L" channels. This is the Windows mix, not the Bluetooth bitrate." : L"Mix format is unavailable.");
    }
    if (!h.node.usesPnA2dp()) { add(HealthLevel::Info, L"Bluetooth telemetry", L"This driver manages the codec. Bluetooth bitrate, packet health and receiver capabilities are not exposed in Windows mode."); return checks; }
    if (!h.statusValid) { add(HealthLevel::Error, L"PN driver", L"Control query failed: " + Hex(h.controlError) + L". Inspect Driver readiness and the selected device.", HealthAction::Driver); return checks; }
    add(HealthLevel::Pass, L"PN driver", L"The PN-A2DP control interface responds.");
    if (h.status.LastError) add(HealthLevel::Warning, L"Last driver error", Hex(h.status.LastError) + L" is the last recorded error; it may predate current playback.", HealthAction::Driver);
    if (h.status.Flags & PNA2DP_STATUS_FLAG_SCO_ACTIVE) add(HealthLevel::Info, L"Call audio", L"A voice connection is active. Stereo playback may be suspended.");
    if (h.status.Codec) {
        bool supported = h.capsValid && SinkSupportsCodec(h.caps, h.status.Codec);
        add(!h.capsValid ? HealthLevel::Unknown : supported ? HealthLevel::Pass : HealthLevel::Warning, L"Receiver codec",
            !h.capsValid ? L"Receiver capabilities could not be read." : std::wstring(CodecName(h.status.Codec)) + (supported ? L" is advertised by the receiver." : L" is not present in the current receiver capability snapshot."), HealthAction::Codecs);
        bool encoder = h.status.Codec < PNA2DP_CODEC_MAX && ((h.version.KernelCodecMask | h.version.HostCodecMask) & PNA2DP_CODEC_BIT(h.status.Codec));
        add(!h.versionValid ? HealthLevel::Unknown : encoder ? HealthLevel::Pass : HealthLevel::Error, L"Encoder", !h.versionValid ? L"Encoder availability could not be queried." : encoder ? L"An encoder is registered for the selected codec." : L"No encoder is available for the selected codec.", HealthAction::Driver);
    } else add(HealthLevel::Info, L"Codec", L"No codec negotiated yet. Connect the receiver and start playback.");
    bool needsHost = h.status.EncoderOwner == 2 || (h.status.Flags & PNA2DP_STATUS_FLAG_HOST_MISSING) ||
        (h.versionValid && h.status.Codec > 0 && h.status.Codec < PNA2DP_CODEC_MAX && !(h.version.KernelCodecMask & PNA2DP_CODEC_BIT(h.status.Codec)));
    add(!h.serviceKnown ? (needsHost ? HealthLevel::Unknown : HealthLevel::Info) : h.serviceState == SERVICE_RUNNING ? HealthLevel::Pass : needsHost ? HealthLevel::Error : HealthLevel::Info,
        L"Codec host", !h.serviceKnown ? L"Service state unavailable. Kernel SBC can operate independently." : std::wstring(ServiceStateName(h.serviceState)) + (needsHost ? L"; this stream requires the codec host." : L"; the current path does not require a host encoder."), HealthAction::Driver);
    if (h.status.Flags & PNA2DP_STATUS_FLAG_FALLBACK_SBC) add(HealthLevel::Warning, L"Codec fallback", L"SBC fallback is active. Inspect codec availability and the codec host.", HealthAction::Driver);
    add(h.statsValid ? recent.level : HealthLevel::Unknown, L"Recent transport", h.statsValid ? recent.recent + L". " + recent.reason : L"Packet counters could not be queried.", HealthAction::Adapter);
    if (!h.qualityValid) add(HealthLevel::Info, L"Adaptive quality", L"Quality telemetry is unavailable; an older driver may not expose it.");
    return checks;
}
std::wstring HealthCheckText(const std::vector<HealthCheck> &checks) {
    std::wstring text;
    for (const auto &check : checks) text += L"[" + std::wstring(HealthLevelName(check.level)) + L"] " + check.name + L": " + check.detail + L"\r\n\r\n";
    return text;
}
std::wstring DiagnosticReport(const HealthSnapshot &h, const QualityHistory &history, ULONGLONG now, const std::wstring &version) {
    std::wostringstream out;
    const auto &t = h.capturedUtc;
    out << L"PN-A2DP diagnostic report (schema 1)\r\nApplication: " << version << L"\r\nCaptured UTC: " << t.wYear << L"-" << std::setfill(L'0') << std::setw(2) << t.wMonth << L"-" << std::setw(2) << t.wDay << L" " << std::setw(2) << t.wHour << L":" << std::setw(2) << t.wMinute << L":" << std::setw(2) << t.wSecond;
    out << L"\r\nPrivacy: device names, Bluetooth addresses, endpoint/instance IDs, paths and raw logs omitted. No audio recorded.\r\n";
    out << L"Mode: " << (h.demo ? L"DEMO" : !h.found ? L"unavailable" : h.node.usesPnA2dp() ? L"PN-A2DP" : L"Windows/system") << L"\r\n\r\n";
    out << HealthCheckText(EvaluateHealth(h, history.Summary(now)));
    out << L"Platform: test signing=" << (h.signing.queried ? h.signing.testSigning ? L"on" : L"off" : L"unknown")
        << L", Secure Boot=" << (h.signing.secureBootKnown ? h.signing.secureBoot ? L"on" : L"off" : L"unknown")
        << L", memory integrity=" << (h.signing.queried ? h.signing.hvci ? L"on" : L"off" : L"unknown") << L". Read-only observations.\r\n";
    if (h.versionValid) out << L"Driver version (packed): " << Hex(h.version.DriverVersion) << L"; ABI " << h.version.AbiVersion << L"; kernel mask " << Hex(h.version.KernelCodecMask) << L"; host mask " << Hex(h.version.HostCodecMask) << L"\r\n";
    if (h.statusValid) out << L"State: " << StateName(h.status.State) << L"; codec: " << CodecName(h.status.Codec) << L"; rate " << h.status.SampleRate << L"; Windows rate " << h.status.EngineSampleRate << L"; flags " << Hex(h.status.Flags) << L"; channels " << h.status.Channels << L"; bitrate " << h.status.BitrateBps << L"; negotiated/configured ceiling " << h.status.MaxBitrateBps << L"; MTU " << h.status.MediaMtu << L"; last error " << Hex(h.status.LastError) << L"\r\n";
    if (h.statsValid) out << L"Lifetime counters (may include older incidents): packets " << h.stats.PacketsSent << L", drops " << h.stats.PacketsDropped << L", write failures " << h.stats.WriteFailures << L", latency trims " << h.stats.Underruns << L", host timeouts " << h.stats.HostTimeouts << L"\r\n";
    if (h.qualityValid) out << L"Quality: state " << h.quality.Health << L", flags " << Hex(h.quality.Flags) << L", healthy ms " << h.quality.HealthyMs << L", congested ms " << h.quality.CongestedMs << L", radio query " << Hex(h.quality.RadioQueryStatus) << L", manufacturer " << h.quality.Manufacturer << L", HCI " << (ULONG)h.quality.HciVersion << L", LMP features " << h.quality.LmpFeatures << L"\r\n";
    if (h.capsValid) {
        out << L"Receiver audio sink capabilities:\r\n";
        for (ULONG i = 0; i < (std::min)(h.caps.SepCount, (ULONG)PNA2DP_MAX_REMOTE_SEPS); ++i) {
            const auto &sep = h.caps.Seps[i];
            if (sep.TsepIsSink && !sep.MediaType) out << L"  codec " << CodecName(sep.Codec) << L", vendor " << Hex(sep.VendorId) << L", codec ID " << sep.VendorCodecId << L", IE " << HexBytes(sep.CodecInfo, (std::min)((ULONG)sep.CodecInfoLength, 32ul)) << L"\r\n";
        }
    }
    if (h.configValid) {
        out << L"\r\nSaved preferences (unapplied edits excluded):\r\n";
        out << L"Priority:";
        for (ULONG codec : h.config.CodecPriority) { if (!codec) break; out << L" " << CodecName(codec); }
        out << L"\r\nEnabled mask: " << Hex(h.config.EnabledCodecMask) << L"\r\n";
        for (ULONG codec = PNA2DP_CODEC_SBC; codec < PNA2DP_CODEC_MAX; ++codec) if (h.config.EnabledCodecMask & PNA2DP_CODEC_BIT(codec)) out << CodecName(codec) << L": " << CodecSettingsText(h.config, codec) << L"\r\n";
        out << L"Flags " << Hex(h.config.Flags) << L"; target/max latency ms " << h.config.TargetLatencyMs << L"/" << h.config.MaxLatencyMs << L"; ABR floor " << h.config.AbrFloorBps << L"; persisted volume dB/65536 " << h.config.VolumeDb16 << L"\r\n";
    }
    out << L"\r\nRecent timeline (up to 60 seconds, resets at gaps/reconnects/counter resets; sampled after the saved check, ages relative to export):\r\nage_ms,codec,bitrate_bps,packets,drops,write_failures,latency_trims\r\n";
    for (const auto &sample : history.Samples()) out << (now >= sample.tick ? now - sample.tick : 0) << L"," << CodecName(sample.status.Codec) << L"," << sample.status.BitrateBps << L"," << sample.stats.PacketsSent << L"," << sample.stats.PacketsDropped << L"," << sample.stats.WriteFailures << L"," << sample.stats.Underruns << L"\r\n";
    out << L"\r\nQuality events (up to 32; observed times; retained across reconnects for this selection):\r\n" << history.EventText(now);
    out << L"\r\nNo acoustic measurement or bandwidth/RSSI test was performed. Unknown data is not a pass.\r\n";
    return out.str();
}
}
