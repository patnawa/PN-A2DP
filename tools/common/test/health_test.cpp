/* Production diagnostic model and read-only machine integration. SPDX-License-Identifier: MIT */
#include "pnhealth.h"
#include <objbase.h>
#include <stdio.h>
#include <algorithm>

static int checks = 0, failures = 0;
#define CHECK(c, message) do { ++checks; if (!(c)) { ++failures; printf("FAIL: %s\n", message); } } while (0)
static bool Has(const std::vector<pn::HealthCheck> &rows, const wchar_t *name, pn::HealthLevel level) {
    for (const auto &row : rows) if (row.name == name && row.level == level) return true;
    return false;
}
static pn::HealthSnapshot Good() {
    pn::HealthSnapshot h;
    h.nodeKnown = h.found = h.node.present = h.node.started = true; h.node.service = PN_DRIVER_SERVICE;
    h.statusValid = h.statsValid = h.versionValid = h.capsValid = h.configValid = h.qualityValid = true;
    h.audio.active = h.audio.defaultKnown = h.audio.isDefault = true;
    h.audio.error = h.audio.formatError = h.audio.volumeError = h.audio.muteError = S_OK;
    h.audio.volume = .5f; h.audio.channels = 2; h.audio.sampleRate = 48000;
    h.status.State = PNA2DP_STATE_STREAMING; h.status.Flags = PNA2DP_STATUS_FLAG_AUDIO_ACTIVE;
    h.status.Codec = PNA2DP_CODEC_SBC; h.status.BitrateBps = h.status.MaxBitrateBps = 357000;
    h.version.KernelCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    h.caps.SepCount = 1; h.caps.Seps[0].TsepIsSink = 1; h.caps.Seps[0].Codec = PNA2DP_CODEC_SBC;
    h.caps.Seps[0].CodecInfoLength = 4;
    UCHAR ie[] = {0x3F, 0xFF, 2, 53}; memcpy(h.caps.Seps[0].CodecInfo, ie, 4);
    h.serviceKnown = true; h.serviceState = SERVICE_RUNNING;
    h.quality.Flags = PNA2DP_QUALITY_ABR; h.quality.Health = PNA2DP_QUALITY_STABLE;
    pn::ConfigDefaults(h.config);
    return h;
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 4 && !wcscmp(argv[1], L"--live")) {
        auto h = pn::CollectHealth(argv[2]); pn::QualityHistory history;
        auto report = pn::DiagnosticReport(h, history, GetTickCount64(), L"integration-test");
        DWORD error = pn::WriteUtf8FileAtomic(argv[3], report);
        wprintf(L"%s\nExport error: %lu\n", pn::HealthCheckText(pn::EvaluateHealth(h, history.Summary(GetTickCount64()))).c_str(), error);
        return error || !h.found || !h.audio.active || (h.node.usesPnA2dp() && !h.statusValid) ? 1 : 0;
    }
    auto h = Good(); pn::QualityHistory history;
    CHECK(history.Summary(0).level == pn::HealthLevel::Unknown, "no data is not a pass");
    for (ULONGLONG i = 0; i <= 4; ++i) {
        h.stats.PacketsSent = i * 100; history.Observe(i * 1000, true, h.status, h.stats, true, h.quality);
    }
    auto recent = history.Summary(4000);
    CHECK(recent.level == pn::HealthLevel::Pass && recent.packets == 400 && recent.seconds == 4, "healthy packet progress");
    CHECK(history.Summary(7000).level == pn::HealthLevel::Unknown, "stale sample not healthy");
    auto rows = pn::EvaluateHealth(h, recent);
    CHECK(Has(rows, L"Audio output", pn::HealthLevel::Pass), "exact endpoint check");
    CHECK(Has(rows, L"Receiver codec", pn::HealthLevel::Pass), "advertised codec recognized");
    CHECK(Has(rows, L"Encoder", pn::HealthLevel::Pass), "kernel SBC encoder recognized");
    auto bad = h; bad.nodeKnown = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Device", pn::HealthLevel::Unknown), "enumeration failure unknown");
    bad = h; bad.found = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Device", pn::HealthLevel::Error), "missing device error");
    bad = h; bad.node.problemCode = 52;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Device", pn::HealthLevel::Error), "device problem reported");
    bad = h; bad.node.restartRequired = true;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Driver change", pn::HealthLevel::Warning), "pending reboot distinguished");
    bad = h; bad.audio.active = false; bad.audio.error = E_FAIL;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Audio output", pn::HealthLevel::Unknown), "failed endpoint query not invented disconnected state");
    bad = h; bad.audio.muted = true;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Volume", pn::HealthLevel::Warning), "mute detected");
    bad = h; bad.audio.volume = 0;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Volume", pn::HealthLevel::Warning), "zero volume detected");
    bad = h; bad.audio.volumeError = E_ACCESSDENIED;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Volume", pn::HealthLevel::Unknown), "failed volume query not shown as zero");
    bad = h; bad.audio.isDefault = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Output selection", pn::HealthLevel::Info), "non-default output can still work per app");
    bad = h; bad.statusValid = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"PN driver", pn::HealthLevel::Error), "failed control query");
    bad = h; bad.version.KernelCodecMask = 0;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Encoder", pn::HealthLevel::Error), "missing encoder detected");
    bad = h; bad.status.Codec = PNA2DP_CODEC_LDAC;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Receiver codec", pn::HealthLevel::Warning), "unsupported receiver codec detected");
    bad = h; bad.capsValid = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Receiver codec", pn::HealthLevel::Unknown), "unavailable caps not unsupported");
    bad = h; bad.serviceKnown = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Codec host", pn::HealthLevel::Info), "kernel SBC does not require host");
    bad.status.EncoderOwner = 2;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Codec host", pn::HealthLevel::Unknown), "missing host query for host codec unknown");
    bad.serviceKnown = true; bad.serviceState = SERVICE_STOPPED;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Codec host", pn::HealthLevel::Error), "stopped required host detected");
    bad.status.EncoderOwner = 0; bad.status.Codec = PNA2DP_CODEC_LDAC; bad.status.State = PNA2DP_STATE_OPEN;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Codec host", pn::HealthLevel::Error), "idle host-only codec still requires service before playback");
    bad = h; bad.statsValid = false;
    CHECK(Has(pn::EvaluateHealth(bad, recent), L"Recent transport", pn::HealthLevel::Unknown), "failed counters invalidate healthy history");
    bad = h; bad.node.service = L"BthA2dp";
    auto windows = pn::EvaluateHealth(bad, recent);
    CHECK(Has(windows, L"Bluetooth telemetry", pn::HealthLevel::Info) && !Has(windows, L"Encoder", pn::HealthLevel::Pass), "Windows mode excludes invented codec health");
    h.stats.PacketsDropped = 1; h.stats.PacketsSent += 100;
    history.Observe(5000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Summary(5000).level == pn::HealthLevel::Warning && history.Summary(5000).drops == 1, "recent drops highlighted");
    h.stats = {}; history.Observe(6000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Samples().size() == 1 && history.Summary(6000).drops == 0, "counter reset never unsigned underflow");
    history.Observe(12000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Samples().size() == 1, "gap restarts observation window");
    history.Observe(13000, false, h.status, h.stats, true, h.quality);
    CHECK(history.Samples().empty(), "failed sample clears old evidence");
    for (ULONGLONG i = 0; i < 100; ++i) { ++h.stats.PacketsSent; history.Observe(i * 1000, true, h.status, h.stats, true, h.quality); }
    CHECK(history.Samples().size() == 61 && history.Summary(99000).seconds == 60, "history bounded to 60 seconds");
    h.quality.Health = PNA2DP_QUALITY_RECOVERING; h.quality.HealthyMs = 15000;
    history.Observe(100000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Summary(100000).reason.find(L"15 s") != std::wstring::npos, "recovery shows remaining clean traffic time");
    h.quality.Health = PNA2DP_QUALITY_CONGESTED; h.quality.Flags = 0;
    history.Observe(101000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Summary(101000).reason.find(L"is off") != std::wstring::npos, "disabled adaptation never promises bitrate reduction");
    h.status.State = PNA2DP_STATE_OPEN;
    history.Observe(102000, true, h.status, h.stats, true, h.quality);
    CHECK(history.Samples().size() == 1 && history.Summary(102000).level == pn::HealthLevel::Info, "idle not healthy playback and clears previous stream");
    h = Good(); history.Reset();
    for (ULONGLONG i = 0; i < 5; ++i) history.Observe(i * 1000, true, h.status, h.stats, false, h.quality);
    CHECK(history.Summary(4000).title == L"No packet progress", "active but stalled stream detected");
    auto resumed = h; resumed.status.Flags = 0;
    history.Observe(5000, true, resumed.status, resumed.stats, false, resumed.quality);
    resumed.status.Flags = PNA2DP_STATUS_FLAG_AUDIO_ACTIVE;
    history.Observe(6000, true, resumed.status, resumed.stats, false, resumed.quality);
    CHECK(history.Samples().size() == 1 && history.Summary(6000).title == L"Measuring playback", "resume cannot inherit idle time and report an immediate stall");
    CHECK(history.Summary(6000).title != L"Measurements paused", "resume timestamp is fresh");
    CHECK(history.Summary(6000).packets == 0, "resume packet delta correctly stays zero");
    h.node.name = h.node.instanceId = h.audio.endpointId = h.audio.endpointName = h.audio.detail = L"PRIVATE_SENTINEL";
    wcscpy_s(h.status.RemoteName, L"PRIVATE_SENTINEL"); h.status.RemoteAddress = 0x112233445566; h.quality.LocalAddress = 0x112233445566;
    auto report = pn::DiagnosticReport(h, history, 4000, L"0.1.0.27");
    CHECK(report.find(L"PRIVATE_SENTINEL") == std::wstring::npos && report.find(L"112233445566") == std::wstring::npos, "allowlist excludes names identifiers addresses and freeform details");
    CHECK(report.find(L"schema 1") != std::wstring::npos && report.find(L"Saved preferences") != std::wstring::npos && report.find(L"age_ms,") != std::wstring::npos, "report includes schema config and timeline");
    CHECK(report.find(L"; Windows rate ") != std::wstring::npos && report.find(L"; flags ") != std::wstring::npos, "report includes the Windows rate the driver converts from (0.1.0.22)");
    h.demo = true;
    CHECK(pn::DiagnosticReport(h, history, 4000, L"test").find(L"DEMO") != std::wstring::npos, "demo report cannot be confused with hardware evidence");
    wchar_t folder[MAX_PATH]{}, path[MAX_PATH]{}; GetTempPathW(MAX_PATH, folder); GetTempFileNameW(folder, L"pnh", 0, path);
    CHECK(pn::WriteUtf8FileAtomic(path, report) == ERROR_SUCCESS, "real UTF-8 report export");
    HANDLE lock = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE && pn::WriteUtf8FileAtomic(path, L"replacement") != ERROR_SUCCESS, "blocked export reports failure");
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    CHECK(pn::WriteUtf8FileAtomic(path, L"") == ERROR_INVALID_DATA, "empty export rejected");
    DeleteFileW(path);
    h = Good(); history.Reset();
    h.quality.Flags |= PNA2DP_QUALITY_CHANGE_DETAILS;
    history.Observe(1000, true, h.status, h.stats, true, h.quality);
    h.status.BitrateBps = 320000; h.quality.ChangeSequence = 1; h.quality.LastChangeReason = PNA2DP_QUALITY_REASON_SLOW;
    history.Observe(2000, true, h.status, h.stats, true, h.quality);
    CHECK(history.EventText(2000).find(L"slow transport writes") != std::wstring::npos, "bitrate event retains driver cause");
    history.Observe(3000, false, {}, {}, false, {});
    CHECK(history.Samples().empty() && history.EventText(3000).find(L"slow transport writes") != std::wstring::npos, "events survive interrupted observations without joining counters");
    history.Observe(4000, true, h.status, h.stats, true, h.quality);
    CHECK(pn::DiagnosticReport(h, history, 4000, L"test").find(L"slow transport writes") != std::wstring::npos, "events included in diagnostic export");
    history.Reset(); CHECK(history.EventText(5000).find(L"slow transport writes") == std::wstring::npos, "changing selection clears event history");
    for (unsigned i=0; i<100; ++i) history.RecordEvent(i, L"bounded event");
    auto eventText = history.EventText(100);
    CHECK(std::count(eventText.begin(), eventText.end(), L'\n') == 32, "event history bounded to 32 entries");
    history.Reset(); h = Good(); h.status.Codec = PNA2DP_CODEC_APTX;
    for (unsigned i=0; i<=10; ++i) history.Observe(i*1000, true, h.status, h.stats, true, h.quality);
    CHECK(history.SuggestSbcFallback(10000), "persistent fixed-rate stall offers user-controlled fallback");
    CHECK(!history.SuggestSbcFallback(15000), "stale evidence cannot offer fallback");
    history.Reset();
    for (unsigned i=0; i<=10; ++i) history.Observe(i*1017, true, h.status, h.stats, true, h.quality);
    CHECK(history.SuggestSbcFallback(10170), "normal timer jitter does not prevent a persistent stall offer");
    history.Reset(); h = Good();
    for (unsigned i=0; i<=10; ++i) history.Observe(i*1000, true, h.status, h.stats, true, h.quality);
    CHECK(!history.SuggestSbcFallback(10000), "SBC never offers a pointless SBC fallback");
    history.Reset(); h = Good(); h.status.Codec = PNA2DP_CODEC_APTX_HD;
    for (unsigned i=0; i<=10; ++i) { h.stats.PacketsSent++; if (i==1) h.stats.WriteFailures++; history.Observe(i*1000, true, h.status, h.stats, true, h.quality); }
    CHECK(!history.SuggestSbcFallback(10000), "one old failure cannot trigger fallback offer");
    h.quality.Flags = PNA2DP_QUALITY_ABR | PNA2DP_QUALITY_CHANGE_DETAILS;
    h.quality.Health = PNA2DP_QUALITY_RECOVERING; h.quality.HealthyMs = 2000; h.quality.RecoveryTargetMs = 5000;
    h.stats.WriteFailures = 0; history.Reset(); history.Observe(0, true, h.status, h.stats, true, h.quality);
    CHECK(history.Summary(0).reason.find(L"3 s") != std::wstring::npos, "countdown uses driver recovery target");

    /* Connect / Disconnect: KSPROPSETID_BtAudio {7FA06C40-B8F6-4C7E-8556-E8C33A12E54D}, a GET as
     * Windows sends it. No request reaches a real device here: the instance below does not exist. */
    static const GUID btAudio = {0x7FA06C40, 0xB8F6, 0x4C7E, {0x85, 0x56, 0xE8, 0xC3, 0x3A, 0x12, 0xE5, 0x4D}};
    auto connect = pn::BluetoothAudioOneShotRequest(true), disconnect = pn::BluetoothAudioOneShotRequest(false);
    CHECK(IsEqualGUID(connect.set, btAudio) && IsEqualGUID(disconnect.set, btAudio), "one-shot property set is KSPROPSETID_BtAudio");
    CHECK(connect.id == 0 && disconnect.id == 1, "ONESHOT_RECONNECT is 0, ONESHOT_DISCONNECT is 1");
    CHECK(connect.flags == 0x1 && disconnect.flags == 0x1, "one-shots are sent as KSPROPERTY_TYPE_GET, as documented");
    using Stage = pn::OneShotStage;
    Stage stage = Stage::Driver;
    CHECK(pn::BluetoothAudioOneShot(L"", true, &stage) == E_INVALIDARG && stage == Stage::Lookup, "no instance, no request");
    if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
        stage = Stage::Driver;
        HRESULT missing = pn::BluetoothAudioOneShot(L"BTHENUM\\PN_A2DP_TEST_NO_SUCH_DEVICE\\0", false, &stage);
        /* read-only enumeration: every active and unplugged endpoint is checked, none matches */
        CHECK(missing == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) && stage == Stage::NoEndpoint, "an unknown device has no endpoint and nothing is sent");
        CoUninitialize();
    } else CHECK(false, "COM initialization for the one-shot lookup");
    CHECK(pn::BluetoothAudioOneShotText(S_OK, Stage::Driver, true, true).find(L"Connect request sent") == 0, "connect success text");
    CHECK(pn::BluetoothAudioOneShotText(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Stage::NoEndpoint, true, true).find(L"no audio output") != std::wstring::npos, "missing endpoint text");
    /* E_NOTFOUND from an unconnected topology connector is ERROR_NOT_FOUND too: it is never "no output" */
    std::wstring topology = pn::BluetoothAudioOneShotText(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Stage::Topology, false, true);
    CHECK(topology.find(L"could not be reached") != std::wstring::npos && topology.find(L"no audio output") == std::wstring::npos &&
          topology.find(L"pair it again") == std::wstring::npos, "topology failure is not reported as a missing output");
    std::wstring lookup = pn::BluetoothAudioOneShotText(HRESULT_FROM_WIN32(ERROR_NOT_FOUND), Stage::Lookup, true, true);
    CHECK(lookup.find(L"no audio output") == std::wstring::npos && lookup.find(L"Retry") != std::wstring::npos, "endpoint check failure asks for a retry");
    CHECK(pn::BluetoothAudioOneShotText(HRESULT_FROM_WIN32(ERROR_INVALID_FUNCTION), Stage::Driver, false, true).find(L"0.1.0.21") != std::wstring::npos, "old SET-only PN-A2DP driver text");
    std::wstring other = pn::BluetoothAudioOneShotText(HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND), Stage::Driver, false, false);
    CHECK(other.find(L"does not accept") != std::wstring::npos && other.find(L"PN-A2DP") == std::wstring::npos, "another driver's rejection does not blame PN-A2DP");
    CHECK(pn::BluetoothAudioOneShotText(E_FAIL, Stage::Driver, false, true).find(L"0x80004005") != std::wstring::npos, "error code shown");
    /* the reconnect fallback resets the link: only for Connect, only without any output, only while the session is down */
    CHECK(pn::OneShotReconnectFallback(true, Stage::NoEndpoint, true, PNA2DP_STATE_IDLE), "fallback for an idle session without output");
    CHECK(pn::OneShotReconnectFallback(true, Stage::NoEndpoint, true, PNA2DP_STATE_ERROR), "fallback for a failed session without output");
    bool resetsLiveSession = false;
    for (ULONG state = PNA2DP_STATE_CONNECTING; state <= PNA2DP_STATE_CLOSING; ++state)
        resetsLiveSession |= pn::OneShotReconnectFallback(true, Stage::NoEndpoint, true, state);
    CHECK(!resetsLiveSession, "never a fallback while connecting, connected, streaming, suspended or closing");
    CHECK(!pn::OneShotReconnectFallback(true, Stage::NoEndpoint, false, PNA2DP_STATE_IDLE), "no fallback when the session state is unknown");
    CHECK(!pn::OneShotReconnectFallback(true, Stage::Topology, true, PNA2DP_STATE_IDLE) && !pn::OneShotReconnectFallback(true, Stage::Lookup, true, PNA2DP_STATE_IDLE) &&
          !pn::OneShotReconnectFallback(true, Stage::Driver, true, PNA2DP_STATE_IDLE), "no fallback unless no endpoint exists");
    CHECK(!pn::OneShotReconnectFallback(false, Stage::NoEndpoint, true, PNA2DP_STATE_IDLE), "Disconnect never falls back to a reconnect");
    printf("Audio health: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
