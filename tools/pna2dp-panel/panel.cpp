/*
 * panel.cpp - PN-A2DP Control Panel.
 *
 * A Win32 application (no framework, builds with the same cl/link invocation as the rest
 * of the tree) that shows every paired Bluetooth stereo device, lets the user switch each
 * one between the inbox Microsoft driver and PN-A2DP, and configures the PN-A2DP driver
 * per headset: codec priority and enable mask, SBC / AAC / aptX / LDAC parameters,
 * buffering, adaptive bitrate and behaviour flags, with live status and statistics.
 *
 *   pna2dp-panel.exe                 interactive
 *   pna2dp-panel.exe --demo          interactive with a simulated headset (UI development)
 *   pna2dp-panel.exe --op <name> ... elevated helper (spawned by the panel through UAC):
 *       use-pna2dp <instanceId> [infPath]   use-inbox <instanceId>   stage <infPath>
 *       unstage <oemNN.inf>   svc-start   svc-stop   svc-install <exePath>   svc-remove   rescan
 *       boot-settings (administrator boot-mode dialog; no change until confirmed)
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>
#include <stdio.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "resource.h"
#include "version.h"
#include "pnctl.h"
#include "pndrv.h"
#include "pnboot.h"
#include "pnaudio.h"
#include "pnretry.h"
#include "pnhealth.h"
#include <memory>
#include "bootdlg.h"
#include <uxtheme.h>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "uxtheme.lib")

#define WM_APP_DRIVER_EVENT   (WM_APP + 1)   /* wParam = event mask */
#define WM_APP_DEVICE_GONE    (WM_APP + 2)
#define WM_APP_LAYOUT         (WM_APP + 3)
#define WM_APP_DEFERRED       (WM_APP + 4)
#define TIMER_STATS           1
#define TIMER_DEMO            2
#define TIMER_HEALTH          3

static HINSTANCE g_inst;

/* ------------------------------------------------------------------------- */
/* Model                                                                        */
/* ------------------------------------------------------------------------- */

struct DeviceEntry {
    pn::A2dpNode node;
    std::wstring ifacePath;     /* non-empty when PN-A2DP serves the node */
    bool demo = false;
};

/* Everything we know about the selected headset. In demo mode the data is simulated. */
struct Session {
    pn::Device dev;
    bool open = false;
    bool demo = false;
    PNA2DP_VERSION ver = {};
    PNA2DP_STATUS status = {};
    PNA2DP_CAPABILITIES caps = {};
    PNA2DP_CONFIG config = {};     /* edited copy */
    PNA2DP_CONFIG applied = {};    /* what the driver has */
    PNA2DP_STATS stats = {};
    bool telemetryValid = false;
    ULONGLONG demoTicks = 0;

    void Reset()
    {
        dev.Close();
        open = demo = false;
        ver = PNA2DP_VERSION{}; status = PNA2DP_STATUS{}; caps = PNA2DP_CAPABILITIES{};
        config = PNA2DP_CONFIG{}; applied = PNA2DP_CONFIG{}; stats = PNA2DP_STATS{};
        demoTicks = 0;
        telemetryValid = false;
    }
    void DemoFill()
    {
        demo = true; open = true; telemetryValid = true;
        ver.AbiVersion = PNA2DP_ABI_VERSION; ver.DriverVersion = 0x00010000; ver.Role = 1;
        ver.KernelCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
        ver.HostCodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC) |
                            PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL);
        status.Size = sizeof(status); status.RemoteAddress = 0xE8EC4A123456ull; wcscpy_s(status.RemoteName, L"WH-1000XM5 (demo)");
        status.State = PNA2DP_STATE_STREAMING; status.Codec = PNA2DP_CODEC_LDAC; status.SampleRate = 48000; status.Channels = 2; status.BitsPerSample = 24;
        status.BitrateBps = 660000; status.MaxBitrateBps = 990000; status.MediaMtu = 1005; status.SinkReportedDelayUs = 150000; status.EstimatedLatencyUs = 212000;
        status.Flags = PNA2DP_STATUS_FLAG_AUDIO_ACTIVE | PNA2DP_STATUS_FLAG_DELAY_REPORTING | PNA2DP_STATUS_FLAG_RADIO_IN_RANGE; status.AvdtpVersion = 0x0103; status.RemoteSeid = 4; status.EncoderOwner = 2;
        caps.Size = sizeof(caps); caps.SdpProfileVersion = 0x0103; caps.SdpSupportedFeatures = 0x000F; caps.SepCount = 4;
        auto sep = [&](int i, UCHAR seid, ULONG codec, ULONG vid, USHORT cid, std::initializer_list<UCHAR> ie, ULONG svc) {
            PNA2DP_REMOTE_SEP &s = caps.Seps[i]; s.Seid = seid; s.TsepIsSink = 1; s.Codec = codec; s.VendorId = vid; s.VendorCodecId = cid; s.ServiceCaps = svc;
            s.CodecInfoLength = (USHORT)ie.size(); int k = 0; for (UCHAR b : ie) s.CodecInfo[k++] = b; caps.CodecMask |= PNA2DP_CODEC_BIT(codec);
        };
        sep(0, 1, PNA2DP_CODEC_SBC, 0, 0, { 0x3F, 0xFF, 0x02, 0x35 }, PNA2DP_SVC_MEDIA_TRANSPORT | PNA2DP_SVC_DELAY_REPORTING);
        sep(1, 2, PNA2DP_CODEC_AAC, 0, 0, { 0xC0, 0x01, 0x8C, 0x84, 0xE2, 0x00 }, PNA2DP_SVC_MEDIA_TRANSPORT | PNA2DP_SVC_DELAY_REPORTING);
        sep(2, 3, PNA2DP_CODEC_APTX, 0x4F, 0x0001, { 0x4F, 0x00, 0x00, 0x00, 0x01, 0x00, 0x32 }, PNA2DP_SVC_MEDIA_TRANSPORT);
        sep(3, 4, PNA2DP_CODEC_LDAC, 0x12D, 0x00AA, { 0x2D, 0x01, 0x00, 0x00, 0xAA, 0x00, 0x3C, 0x07 }, PNA2DP_SVC_MEDIA_TRANSPORT | PNA2DP_SVC_DELAY_REPORTING);
        caps.Seps[3].InUse = 1;
        pn::ConfigDefaults(applied); config = applied;
        stats.Size = sizeof(stats); stats.PcmFramesConsumed = 48000ull * 125; stats.EncodedFrames = 46875; stats.PacketsSent = 15625; stats.BytesSent = 10312500;
        stats.QueueDepthPackets = 2; stats.QueueDepthMaxPackets = 5; stats.TxCompletionAvgUs = 1800; stats.TxCompletionMaxUs = 11200; stats.HostRoundTripAvgUs = 950;
    }
    void DemoTick()
    {
        demoTicks++;
        stats.PcmFramesConsumed += 48000; stats.PacketsSent += 125; stats.BytesSent += 82500; stats.EncodedFrames += 375;
        stats.TxCompletionAvgUs = 1500 + (ULONG)((demoTicks * 97) % 900);
        stats.QueueDepthPackets = 1 + (ULONG)(demoTicks % 4);
        status.EstimatedLatencyUs = 200000 + (ULONG)((demoTicks * 37) % 15000);
    }
    bool Refresh(bool withCaps)
    {
        if (demo) return true;
        if (!open) return false;
        bool ok = dev.GetStatus(status);
        bool statsOk = dev.GetStats(stats);
        telemetryValid = ok && statsOk && status.Size == sizeof(status) && stats.Size == sizeof(stats);
        if (withCaps) dev.GetCapabilities(caps);
        return ok;
    }
    bool LoadConfig()
    {
        if (demo) { config = applied; return true; }
        if (!dev.GetConfig(applied)) return false;
        config = applied;
        return true;
    }
    bool StoreConfig(const PNA2DP_CONFIG &c)
    {
        if (demo) { applied = c; config = c; return true; }
        if (!dev.SetConfig(c)) return false;
        return LoadConfig();
    }
};

/* ------------------------------------------------------------------------- */
/* Helpers                                                                      */
/* ------------------------------------------------------------------------- */

static void SetText(HWND dlg, int id, const std::wstring &s) { SetDlgItemTextW(dlg, id, s.c_str()); }
static void SetTextF(HWND dlg, int id, const wchar_t *fmt, ...)
{
    wchar_t b[512];
    va_list ap; va_start(ap, fmt); _vsnwprintf_s(b, _TRUNCATE, fmt, ap); va_end(ap);
    SetDlgItemTextW(dlg, id, b);
}
static std::wstring GetText(HWND dlg, int id)
{
    wchar_t b[1024] = {};
    GetDlgItemTextW(dlg, id, b, 1024);
    return b;
}
static ULONG GetUInt(HWND dlg, int id) { return (ULONG)_wtol(GetText(dlg, id).c_str()); }
static void SetUInt(HWND dlg, int id, ULONG v) { SetDlgItemInt(dlg, id, v, FALSE); }
static bool Checked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }
static void Check(HWND dlg, int id, bool v) { CheckDlgButton(dlg, id, v ? BST_CHECKED : BST_UNCHECKED); }
static void EnableItem(HWND dlg, int id, bool v) { EnableWindow(GetDlgItem(dlg, id), v); }

/* An empty preference mask means automatic selection, not "nothing enabled".
 * Auto clears restrictions; selecting any individual option leaves Auto. */
static void SyncAutoGroup(HWND dlg, int autoId, int first, int last, int changed = 0)
{
    if (changed == autoId && Checked(dlg, autoId))
        for (int id = first; id <= last; ++id) Check(dlg, id, false);
    bool any = false;
    for (int id = first; id <= last; ++id) any |= Checked(dlg, id);
    Check(dlg, autoId, !any);
}

static void SyncAutoGroups(HWND dlg, int changed = 0)
{
    const int groups[][3] = {
        {IDC_SBC_RAUTO, IDC_SBC_R16, IDC_SBC_R48}, {IDC_SBC_CAUTO, IDC_SBC_CMONO, IDC_SBC_CJOINT},
        {IDC_SBC_BAUTO, IDC_SBC_B4, IDC_SBC_B16}, {IDC_SBC_SAUTO, IDC_SBC_S4, IDC_SBC_S8},
        {IDC_SBC_AAUTO, IDC_SBC_ALOUD, IDC_SBC_ASNR},
        {IDC_AAC_RAUTO, IDC_AAC_R16, IDC_AAC_R96}, {IDC_AAC_CAUTO, IDC_AAC_CMONO, IDC_AAC_CSTEREO},
        {1510, 1500, 1503}, {1530, 1520, 1523}, {1550, 1540, 1543},
        {IDC_LDAC_RAUTO, IDC_LDAC_R44, IDC_LDAC_R96}, {IDC_LDAC_CAUTO, IDC_LDAC_CMONO, IDC_LDAC_CSTEREO}
    };
    for (const auto &g : groups) if (GetDlgItem(dlg, g[0])) SyncAutoGroup(dlg, g[0], g[1], g[2], changed);
}

static void EnableAllChildren(HWND dlg, bool enable)
{
    for (HWND c = GetWindow(dlg, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) EnableWindow(c, enable);
}

static void ListViewAddColumn(HWND lv, int idx, const wchar_t *text, int width)
{
    LVCOLUMNW c = {};
    c.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    c.pszText = (LPWSTR)text; c.cx = width; c.iSubItem = idx;
    ListView_InsertColumn(lv, idx, &c);
}
static int ListViewAddRow(HWND lv, const std::wstring &text, LPARAM param = 0)
{
    LVITEMW it = {};
    it.mask = LVIF_TEXT | LVIF_PARAM;
    it.iItem = ListView_GetItemCount(lv);
    it.pszText = (LPWSTR)text.c_str();
    it.lParam = param;
    return ListView_InsertItem(lv, &it);
}
static void ListViewSet(HWND lv, int row, int col, const std::wstring &text)
{
    ListView_SetItemText(lv, row, col, (LPWSTR)text.c_str());
}
static LPARAM ListViewParam(HWND lv, int row)
{
    LVITEMW it = {}; it.mask = LVIF_PARAM; it.iItem = row;
    ListView_GetItem(lv, &it);
    return it.lParam;
}
static int ListViewSelected(HWND lv) { return ListView_GetNextItem(lv, -1, LVNI_SELECTED); }

static std::wstring Thousands(ULONGLONG v)
{
    wchar_t b[64], out[80];
    swprintf_s(b, L"%llu", v);
    int n = (int)wcslen(b), o = 0;
    for (int i = 0; i < n; i++) { out[o++] = b[i]; int rem = n - i - 1; if (rem && rem % 3 == 0) out[o++] = L','; }
    out[o] = 0;
    return out;
}

static void ShowError(HWND owner, const std::wstring &what, DWORD err)
{
    std::wstring m = what + L"\n\n" + pn::Win32ErrorText(err);
    MessageBoxW(owner, m.c_str(), L"PN-A2DP", MB_ICONERROR | MB_OK);
}

static int Msg(HWND owner, const std::wstring &text, UINT flags = MB_ICONINFORMATION | MB_OK)
{
    return MessageBoxW(owner, text.c_str(), L"PN-A2DP", flags);
}

/* ------------------------------------------------------------------------- */
/* Elevated operations (run in a separate process through UAC)                  */
/* ------------------------------------------------------------------------- */

static std::wstring FindPackageInf()
{
    std::wstring dir = pn::ModuleDir();
    const wchar_t *cands[] = { L"driver\\pna2dp.inf", L"package\\pna2dp.inf", L"..\\package\\pna2dp.inf", L"pna2dp.inf" };
    for (auto c : cands) { std::wstring p = pn::JoinPath(dir, c); if (pn::FileExists(p)) return p; }
    std::wstring inst = pn::JoinPath(pn::InstallDir(), L"driver\\pna2dp.inf");
    if (pn::FileExists(inst)) return inst;
    return L"";
}

static HWND BootSettingsWindow() { return FindWindowW(L"#32770", L"PN-A2DP - Windows boot mode"); }
static DWORD ActivationBlocked(const std::wstring &detail)
{
    Msg(nullptr, L"PN driver activation was stopped before changing the device.\n\n" + detail, MB_ICONWARNING);
    return ERROR_REQUEST_ABORTED; // the helper has already shown the specific reason
}

static DWORD RunOperation(int argc, wchar_t **argv)
{
    if (argc < 1) return ERROR_INVALID_PARAMETER;
    std::wstring op = argv[0];
    bool reboot = false;
    DWORD err = ERROR_SUCCESS;
    if (op == L"boot-settings" && argc == 1) {
        if (!pn::IsElevated()) return ERROR_ACCESS_DENIED;
        return ShowBootModeDialog(g_inst, nullptr);
    }
    if (op == L"use-pna2dp" && argc >= 2) {
        if (BootSettingsWindow()) return ActivationBlocked(L"Close Windows / Test Mode before activating the PN driver.");
        pn::BootDriverOperationLock operation;
        if (operation.error) return ActivationBlocked(L"Another boot-settings window or driver operation is open. Close it and retry.");
        std::wstring inf;
        auto staged = pn::FindStagedPackages();
        std::wstring candidate = argc >= 3 ? argv[2] : !staged.empty() ? staged.back().fullPath : FindPackageInf();
        if (candidate.empty()) return ERROR_FILE_NOT_FOUND;
        auto readiness = pn::EvaluateActivationReadiness(pn::QueryBootMode(), pn::VerifyPnInf(candidate));
        if (!readiness.ready()) return ActivationBlocked(readiness.detail);
        if (argc >= 3 || staged.empty()) {
            pn::StagedPackage sp;
            err = pn::StagePackage(candidate, sp);
            if (err != ERROR_SUCCESS) return err;
            inf = sp.fullPath;
        } else {
            inf = staged.back().fullPath;
        }
        readiness = pn::EvaluateActivationReadiness(pn::QueryBootMode(), pn::VerifyPnInf(inf));
        if (!readiness.ready()) return ActivationBlocked(readiness.detail);
        err = pn::InstallInfOnDevice(argv[1], inf, reboot);
    } else if (op == L"use-inbox" && argc >= 2) {
        if (BootSettingsWindow()) return ERROR_BUSY;
        pn::BootDriverOperationLock operation;
        if (operation.error) return operation.error;
        err = pn::RestoreInboxDriver(argv[1], reboot);
    } else if (op == L"stage" && argc >= 2) {
        pn::StagedPackage sp;
        err = pn::StagePackage(argv[1], sp);
    } else if (op == L"unstage" && argc >= 2) {
        err = pn::UnstagePackage(argv[1]);
    } else if (op == L"svc-start") {
        err = pn::StartServiceByName(PN_HOST_SERVICE);
    } else if (op == L"svc-stop") {
        err = pn::StopServiceByName(PN_HOST_SERVICE);
    } else if (op == L"svc-install" && argc >= 2) {
        err = pn::CreateHostService(argv[1]);
        if (err == ERROR_SUCCESS) err = pn::StartServiceByName(PN_HOST_SERVICE);
    } else if (op == L"svc-remove") {
        err = pn::DeleteServiceByName(PN_HOST_SERVICE);
    } else if (op == L"rescan") {
        err = pn::RescanDevices();
    } else {
        err = ERROR_INVALID_PARAMETER;
    }
    if (err == ERROR_SUCCESS && reboot) err = ERROR_SUCCESS_REBOOT_REQUIRED;
    return err;
}

/* Keep operations in a helper even for an administrator-launched panel, so
 * SetupAPI and service waits never run on the panel's message thread. */
static DWORD Elevated(HWND owner, const std::wstring &args)
{
    DWORD code = 0;
    DWORD err = pn::RunElevated(pn::ModulePath(), L"--op " + args, code, owner);
    if (err != ERROR_SUCCESS) return err;
    return code;
}

static std::wstring Quote(const std::wstring &s) { return L"\"" + s + L"\""; }

/* ------------------------------------------------------------------------- */
/* Panel                                                                        */
/* ------------------------------------------------------------------------- */

enum Page { PG_STATUS = 0, PG_CODECS, PG_SBC, PG_AAC, PG_APTX, PG_LDAC, PG_TRANSPORT, PG_SINK, PG_DRIVER, PG_ADAPTER, PG_WINDOWS, PG_QUALITY, PG_COUNT };
static const struct { int dlg; const wchar_t *title; } kPages[PG_COUNT] = {
    { IDD_PAGE_STATUS, L"Status" }, { IDD_PAGE_CODECS, L"Codecs" }, { IDD_PAGE_SBC, L"SBC" }, { IDD_PAGE_AAC, L"AAC" }, { IDD_PAGE_APTX, L"aptX" },
    { IDD_PAGE_LDAC, L"LDAC" }, { IDD_PAGE_TRANSPORT, L"Transport" }, { IDD_PAGE_SINK, L"Headset" }, { IDD_PAGE_DRIVER, L"Driver" }, { IDD_PAGE_ADAPTER, L"Adapter" }, { IDD_PAGE_WINDOWS, L"Audio" }, { IDD_PAGE_QUALITY, L"Quality" } };

static const ULONG kCodecOrder[] = { PNA2DP_CODEC_LDAC, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_LL, PNA2DP_CODEC_SBC };

class Panel {
public:
    ~Panel();
    bool Create(bool demo);
    int Run();
    void ShowPage(int p) { SelectPage(p); }
    static INT_PTR CALLBACK MainProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK AboutProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TabProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

private:
    HWND m_hwnd = NULL, m_tab = NULL, m_devList = NULL;
    HWND m_page[PG_COUNT] = {};
    int m_cur = PG_STATUS;
    bool m_demo = false;
    std::vector<DeviceEntry> m_devices;
    int m_sel = -1;
    Session m_s;
    pn::QualityHistory m_history;
    std::wstring m_historyId;
    ULONGLONG m_fallbackRequestedAt = 0;
    pn::HealthSnapshot m_health;
    bool m_healthValid = false;
    pn::HealthAction m_healthAction = pn::HealthAction::None;
    struct HealthJob {
        unsigned generation = 0;
        pn::HealthSnapshot result;
        std::atomic<bool> done{false};
        bool failed = false;
    };
    std::shared_ptr<HealthJob> m_healthJob;
    pn::WindowsAudioInfo m_audio;
    std::vector<int> m_tabs;
    bool m_adjustingVolume = false;
    bool m_audioBusy = false;
    unsigned m_audioRevision = 0;
    ULONGLONG m_driverRefreshTick = 0;
    bool m_loading = false;
    bool m_dirty = false;
    std::thread m_evThread;
    std::atomic<bool> m_evStop{ false };
    pn::EventRetryLimiter m_eventRetry;
    unsigned m_sessionGen = 1;      /* bumped whenever a session closes; tags event-thread messages */
    bool m_inPrompt = false;        /* a modal prompt is up: defer refreshes until it is answered */
    bool m_refreshPending = false;
    ULONG m_pendingEvents = 0;
    DWORD m_pendingGone = ERROR_SUCCESS;
    // Common dialogs and COM can dispatch device messages before they return.
    struct ModalScope {
        Panel &panel;
        bool previous;
        explicit ModalScope(Panel &p) : panel(p), previous(p.m_inPrompt) { panel.m_inPrompt = true; }
        ~ModalScope() {
            panel.m_inPrompt = previous;
            if (!previous && (panel.m_refreshPending || panel.m_pendingEvents || panel.m_pendingGone))
                PostMessageW(panel.m_hwnd, WM_APP_DEFERRED, 0, panel.m_sessionGen);
        }
    };
    int Prompt(std::wstring text, UINT flags = MB_ICONINFORMATION) {
        ModalScope modal(*this);
        return Msg(m_hwnd, text, flags);
    }
    void Error(std::wstring text, DWORD error) {
        ModalScope modal(*this);
        ShowError(m_hwnd, text, error);
    }
    LONG m_volumeLoaded = 0;        /* VolumeDb16 shown on the Transport page; written back only when edited */
    bool m_volumeEdited = false;
    pn::PackageTrust m_driverTrust;
    pn::DriverReadiness m_readiness;
    pn::SigningState m_signing;
    bool m_canSwitchPn = false;
    bool m_canSwitchWindows = false;
    struct ControlRect { HWND window; RECT rect; };
    std::vector<ControlRect> m_controls[PG_COUNT];
    SIZE m_pageBase[PG_COUNT] = {};
    HFONT m_titleFont = NULL, m_bodyFont = NULL, m_boldFont = NULL;
    HBRUSH m_background = NULL, m_surface = NULL;
    HIMAGELIST m_deviceRows = NULL;
    UINT m_styleDpi = 0;
    bool m_highContrast = false;
    COLORREF m_bgColor = RGB(244, 247, 251), m_surfaceColor = RGB(255, 255, 255);
    COLORREF m_textColor = RGB(24, 38, 57), m_mutedColor = RGB(91, 106, 125), m_accentColor = RGB(29, 78, 216);

    /* lifecycle */
    void OnInit();
    void OnClose();
    void LayoutPages();
    void InitAppearance();
    void CapturePageLayouts();
    void LayoutShell();
    void UpdateOutputSummary();
    bool WindowsMode() const;
    bool WindowsPending() const { return WindowsMode() && (m_devices[m_sel].node.restartRequired || !m_devices[m_sel].ifacePath.empty()); }
    void ConfigureTabs();
    void LoadWindowsAudio();
    int TabPage(int index) const { return index >= 0 && index < (int)m_tabs.size() ? m_tabs[index] : -1; }
    void DrawControl(const DRAWITEMSTRUCT &draw);
    LRESULT DrawDevices(NMLVCUSTOMDRAW &draw);
    INT_PTR ControlColor(UINT message, HDC dc, HWND control);
    void SelectPage(int p);
    void RefreshDevices(bool keepSelection = true);
    void SelectDevice(int index);
    void CloseSession();
    void StartEventThread();
    void StopEventThread();
    void OnDriverEvent(ULONG mask);
    void SetStatusText(const std::wstring &s) { SetText(m_hwnd, IDC_STATUSTEXT, s); }
    void SetDirty(bool d);
    void EnableConfigPages(bool enable);
    void UpdateCodecAvailability();

    /* commands */
    bool Apply();
    void Revert();
    void Defaults();
    void ImportConfig();
    void ExportConfig();

    /* pages */
    void LoadAll();
    void LoadStatus();
    void LoadCodecs();  void SaveCodecs(PNA2DP_CONFIG &c);
    void UpdateCodecLive();
    void UpdateCodecDetails();
    void LoadSbc();     void SaveSbc(PNA2DP_CONFIG &c);     void UpdateSbcBitrate();
    void LoadAac();     void SaveAac(PNA2DP_CONFIG &c);
    void LoadAptx();    void SaveAptx(PNA2DP_CONFIG &c);
    void LoadLdac();    void SaveLdac(PNA2DP_CONFIG &c);    void UpdateLdacBitrate();
    void LoadTransport(); void SaveTransport(PNA2DP_CONFIG &c);
    void LoadSink();
    void LoadDriver();
    void LoadReadiness();
    void LoadAdapter();
    void LoadQuality(bool observe = false);
    void ShowQualityEvents();
    bool CanOfferFallback() const;
    void CheckAudio();
    void FinishAudioCheck();
    void ExportDiagnostics();
    void OpenHealthAction();
    bool CollectConfig(PNA2DP_CONFIG &c);

    /* page events */
    INT_PTR OnPageCommand(HWND page, int id, int code, HWND ctl);
    INT_PTR OnPageNotify(HWND page, NMHDR *nm);
    INT_PTR OnPageScroll(HWND page, HWND ctl);
    void CodecMove(int delta);
    void DriverOp(const std::wstring &what, const std::wstring &args, bool rescanAfter);
    friend INT_PTR CALLBACK PageProcThunk(HWND, UINT, WPARAM, LPARAM);
};

static Panel *g_panel;

/* ------------------------------------------------------------------------- */

Panel::~Panel()
{
    if (m_titleFont) DeleteObject(m_titleFont);
    if (m_bodyFont) DeleteObject(m_bodyFont);
    if (m_boldFont) DeleteObject(m_boldFont);
    if (m_background) DeleteObject(m_background);
    if (m_surface) DeleteObject(m_surface);
    if (m_deviceRows) ImageList_Destroy(m_deviceRows);
}

void Panel::InitAppearance()
{
    const UINT dpi = GetDpiForWindow(m_hwnd);
    HIGHCONTRASTW hc = {sizeof(hc)};
    SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0);
    m_highContrast = (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
    m_bgColor = m_highContrast ? GetSysColor(COLOR_WINDOW) : RGB(244, 247, 251);
    m_surfaceColor = m_highContrast ? GetSysColor(COLOR_WINDOW) : RGB(255, 255, 255);
    m_textColor = m_highContrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(24, 38, 57);
    m_mutedColor = m_highContrast ? GetSysColor(COLOR_WINDOWTEXT) : RGB(91, 106, 125);
    m_accentColor = m_highContrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(29, 78, 216);
    if (m_background) DeleteObject(m_background);
    if (m_surface) DeleteObject(m_surface);
    m_background = CreateSolidBrush(m_bgColor); m_surface = CreateSolidBrush(m_surfaceColor);
    auto font = [&](int points, int weight) {
        return CreateFontW(-MulDiv(points, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT oldTitle = m_titleFont, oldBody = m_bodyFont, oldBold = m_boldFont;
    m_titleFont = font(18, FW_SEMIBOLD); m_bodyFont = font(10, FW_NORMAL); m_boldFont = font(10, FW_SEMIBOLD);
    for (int id : {IDC_BRAND, IDC_OUTPUT_TITLE}) SendDlgItemMessageW(m_hwnd, id, WM_SETFONT, (WPARAM)m_titleFont, TRUE);
    for (int id : {IDC_BRAND_SUB, IDC_OUTPUT_SUMMARY, IDC_SAVE_HINT, IDC_STATUSTEXT, IDC_REFRESH, IDC_ABOUT, IDC_REVERT, IDC_DEFAULTS, IDC_IMPORT, IDC_EXPORT})
        SendDlgItemMessageW(m_hwnd, id, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
    for (int id : {IDC_DEVICE_LABEL, IDC_BRAND_VERSION, IDC_APPLY}) SendDlgItemMessageW(m_hwnd, id, WM_SETFONT, (WPARAM)m_boldFont, TRUE);
    SendMessageW(m_devList, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
    SendMessageW(m_tab, WM_SETFONT, (WPARAM)m_bodyFont, TRUE);
    TabCtrl_SetPadding(m_tab, MulDiv(8, dpi, 96), MulDiv(8, dpi, 96));
    if (oldTitle) DeleteObject(oldTitle);
    if (oldBody) DeleteObject(oldBody);
    if (oldBold) DeleteObject(oldBold);
    if (m_deviceRows) ImageList_Destroy(m_deviceRows);
    m_deviceRows = ImageList_Create(1, MulDiv(124, dpi, 96), ILC_COLOR32, 1, 1);
    ListView_SetImageList(m_devList, m_deviceRows, LVSIL_SMALL);
    ListView_SetBkColor(m_devList, m_bgColor);
    ListView_SetTextBkColor(m_devList, m_bgColor);
    ListView_SetTextColor(m_devList, m_textColor);
    SetWindowTheme(m_devList, m_highContrast ? NULL : L"Explorer", NULL);
    for (HWND page : m_page) {
        if (!page) continue;
        for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            wchar_t cls[32]; GetClassNameW(child, cls, _countof(cls));
            if (!wcscmp(cls, WC_LISTVIEWW)) {
                SetWindowTheme(child, m_highContrast ? NULL : L"Explorer", NULL);
                ListView_SetBkColor(child, m_surfaceColor); ListView_SetTextBkColor(child, m_surfaceColor);
                ListView_SetTextColor(child, m_textColor);
            }
        }
    }
    m_styleDpi = dpi;
    SendDlgItemMessageW(m_page[PG_QUALITY], IDC_QUALITY_TITLE, WM_SETFONT, (WPARAM)m_boldFont, TRUE);
    SendDlgItemMessageW(m_page[PG_QUALITY], IDC_QUALITY_BITRATE, WM_SETFONT, (WPARAM)m_boldFont, TRUE);
    InvalidateRect(m_hwnd, NULL, TRUE);
}

void Panel::CapturePageLayouts()
{
    const UINT dpi = GetDpiForWindow(m_hwnd);
    for (int i = 0; i < PG_COUNT; ++i) {
        RECT r; GetClientRect(m_page[i], &r);
        m_pageBase[i] = {MulDiv(r.right, 96, dpi), MulDiv(r.bottom, 96, dpi)};
        for (HWND child = GetWindow(m_page[i], GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            GetWindowRect(child, &r); MapWindowPoints(NULL, m_page[i], (POINT *)&r, 2);
            r = {MulDiv(r.left, 96, dpi), MulDiv(r.top, 96, dpi), MulDiv(r.right, 96, dpi), MulDiv(r.bottom, 96, dpi)};
            m_controls[i].push_back({child, r});
        }
    }
}

void Panel::LayoutShell()
{
    if (!m_styleDpi || !m_tab) return;
    const UINT dpi = GetDpiForWindow(m_hwnd);
    auto px = [&](int value) { return MulDiv(value, dpi, 96); };
    RECT client; GetClientRect(m_hwnd, &client);
    int w = MulDiv(client.right, 96, dpi), h = MulDiv(client.bottom, 96, dpi);
    // Keep controls readable on large monitors instead of stretching buttons
    // across the entire desktop. Small windows use all available space.
    int bodyWidth = min(w, 1476), offset = (w - bodyWidth) / 2;
    const int side = 276, content = offset + 316, right = offset + bodyWidth - 20, footer = min(h - 96, 860);
    auto place = [&](int id, int x, int y, int width, int height) {
        SetWindowPos(GetDlgItem(m_hwnd, id), NULL, px(x), px(y), px(width), px(height), SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
    };
    place(IDC_BRAND, offset + 20, 16, side, 32);
    place(IDC_BRAND_VERSION, offset + 20, 49, side, 20);
    place(IDC_BRAND_SUB, offset + 20, 73, side, 22);
    place(IDC_DEVICE_LABEL, offset + 20, 103, side, 22);
    place(IDC_DEVICES, offset + 16, 134, side + 8, footer - 146);
    ListView_SetColumnWidth(m_devList, 0, px(side + 8) - GetSystemMetricsForDpi(SM_CXVSCROLL, dpi) - px(4));
    place(IDC_REFRESH, offset + 20, footer + 34, 128, 34); place(IDC_ABOUT, offset + 160, footer + 34, 128, 34);
    place(IDC_OUTPUT_TITLE, content + 12, 22, right - content - 24, 34);
    place(IDC_OUTPUT_SUMMARY, content + 12, 64, right - content - 24, 34);
    place(IDC_TAB, content, 110, right - content, footer - 122);
    place(IDC_SAVE_HINT, content, footer + 5, right - content, 22);
    place(IDC_DEFAULTS, content, footer + 34, 92, 34);
    place(IDC_IMPORT, content + 100, footer + 34, 84, 34);
    place(IDC_EXPORT, content + 192, footer + 34, 84, 34);
    place(IDC_REVERT, right - 286, footer + 34, 132, 34);
    place(IDC_APPLY, right - 144, footer + 34, 144, 34);
    place(IDC_STATUSTEXT, 20, h - 22, w - 40, 20);
    LayoutPages();
    // Moving sibling windows can copy old pixels into their new rectangles.
    // Repaint the entire child hierarchy after all geometry has settled.
    RedrawWindow(m_hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

void Panel::UpdateOutputSummary()
{
    std::wstring name = L"Select a Bluetooth device", detail = L"Pair a device in Windows, then choose Refresh.";
    if (m_sel >= 0 && m_sel < (int)m_devices.size()) {
        name = m_devices[m_sel].node.name;
        if (name.empty()) name = pn::BtAddressText(m_devices[m_sel].node.address);
        if (m_s.open) {
            const auto &s = m_s.status;
            detail = pn::StateName(s.State);
            if (s.Codec && s.State >= PNA2DP_STATE_CONFIGURED && s.State <= PNA2DP_STATE_SUSPENDED) {
                detail += L"  |  " + std::wstring(pn::CodecName(s.Codec)) + L"  |  " + std::to_wstring(s.BitrateBps / 1000) +
                    L" kbps  |  " + std::to_wstring(s.SampleRate) + L" Hz";
            }
            detail += (m_s.applied.Flags & PNA2DP_CFG_AUTO_QUALITY) ? L"  |  Auto quality" : L"  |  Manual quality";
        } else if (WindowsMode()) {
            detail = m_devices[m_sel].node.usesWindowsA2dp() ? L"Windows mode" : L"System audio - other driver";
            detail += WindowsPending() ? L"  |  Restart required to finish driver change" : m_audio.active ? L"  |  Connected  |  Codec managed by driver" : L"  |  Output unavailable";
        } else detail = L"PN-A2DP control interface unavailable. Check the Driver tab.";
    }
    if (GetText(m_hwnd, IDC_OUTPUT_TITLE) != name) SetText(m_hwnd, IDC_OUTPUT_TITLE, name);
    if (GetText(m_hwnd, IDC_OUTPUT_SUMMARY) != detail) SetText(m_hwnd, IDC_OUTPUT_SUMMARY, detail);
}

INT_PTR Panel::ControlColor(UINT message, HDC dc, HWND control)
{
    if (!m_background) return FALSE;
    bool shell = GetParent(control) == m_hwnd;
    SetBkColor(dc, shell ? m_bgColor : m_surfaceColor);
    SetTextColor(dc, shell && GetDlgCtrlID(control) != IDC_OUTPUT_TITLE && GetDlgCtrlID(control) != IDC_BRAND ? m_mutedColor : m_textColor);
    if (shell && GetDlgCtrlID(control) == IDC_BRAND_VERSION) SetTextColor(dc, m_highContrast ? m_textColor : m_accentColor);
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLORBTN) SetBkMode(dc, TRANSPARENT);
    return (INT_PTR)(shell ? m_background : m_surface);
}

void Panel::DrawControl(const DRAWITEMSTRUCT &d)
{
    const UINT dpi = GetDpiForWindow(m_hwnd);
    RECT r = d.rcItem;
    bool selected = (d.itemState & ODS_SELECTED) != 0, disabled = (d.itemState & ODS_DISABLED) != 0;
    if (d.CtlType == ODT_STATIC && d.CtlID == IDC_QUALITY_GRAPH) {
        FillRect(d.hDC, &r, m_surface);
        const auto &samples = m_history.Samples();
        SetBkMode(d.hDC, TRANSPARENT); SetTextColor(d.hDC, m_mutedColor);
        HGDIOBJ font = SelectObject(d.hDC, m_bodyFont);
        RECT label = r; label.bottom = label.top + MulDiv(18, dpi, 96);
        DrawTextW(d.hDC, WindowsMode() ? L"Bluetooth telemetry" : L"Bitrate history - up to 60 seconds (kbps); orange marks reductions or errors", -1, &label, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT plot = r; plot.top = label.bottom + 3; plot.bottom -= MulDiv(17, dpi, 96);
        plot.left += MulDiv(38, dpi, 96);
        if (samples.size() < 2 || GetTickCount64() - samples.back().tick > 2500) {
            DrawTextW(d.hDC, WindowsMode() ? L"This driver does not expose bitrate history" : L"Waiting for fresh measurements", -1, &plot, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        } else {
            ULONG ceiling = 1;
            for (const auto &s : samples) ceiling = (std::max)(ceiling, (std::max)(s.status.BitrateBps, s.status.MaxBitrateBps));
            RECT axis = r; axis.top = plot.top; axis.right = plot.left - 4;
            std::wstring high = std::to_wstring(ceiling / 1000);
            DrawTextW(d.hDC, high.c_str(), -1, &axis, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            axis.top = plot.bottom - MulDiv(16, dpi, 96); DrawTextW(d.hDC, L"0", -1, &axis, DT_RIGHT | DT_TOP | DT_SINGLELINE);
            axis = plot; axis.top = plot.bottom + 2; axis.bottom = r.bottom;
            DrawTextW(d.hDC, L"60 s ago", -1, &axis, DT_LEFT | DT_SINGLELINE);
            DrawTextW(d.hDC, L"Now", -1, &axis, DT_RIGHT | DT_SINGLELINE);
            HPEN grid = CreatePen(PS_DOT, 1, m_highContrast ? m_textColor : RGB(201, 210, 224));
            HGDIOBJ oldGrid = SelectObject(d.hDC, grid);
            for (int y : {plot.top, (plot.top + plot.bottom) / 2, plot.bottom}) { MoveToEx(d.hDC, plot.left, y, nullptr); LineTo(d.hDC, plot.right, y); }
            SelectObject(d.hDC, oldGrid); DeleteObject(grid);
            HPEN line = CreatePen(PS_SOLID, MulDiv(2, dpi, 96), m_highContrast ? m_textColor : m_accentColor);
            HGDIOBJ old = SelectObject(d.hDC, line);
            ULONGLONG latest = samples.back().tick;
            POINT previous{};
            for (size_t i = 0; i < samples.size(); ++i) {
                const auto &s = samples[i];
                int x = plot.right - 4 - (int)((latest - s.tick) * (plot.right - plot.left - 8) / 60000);
                int y = plot.bottom - 2 - (int)((ULONGLONG)s.status.BitrateBps * (plot.bottom - plot.top - 4) / ceiling);
                if (!i) MoveToEx(d.hDC, x, y, nullptr);
                else {
                    LineTo(d.hDC, x, previous.y); LineTo(d.hDC, x, y);
                    const auto &before = samples[i - 1];
                    if (s.status.BitrateBps < before.status.BitrateBps || s.stats.PacketsDropped > before.stats.PacketsDropped || s.stats.WriteFailures > before.stats.WriteFailures || s.stats.Underruns > before.stats.Underruns) {
                        HPEN marker = CreatePen(PS_SOLID, MulDiv(2, dpi, 96), m_highContrast ? m_textColor : RGB(190, 95, 10));
                        SelectObject(d.hDC, marker); MoveToEx(d.hDC, x, plot.top, nullptr); LineTo(d.hDC, x, plot.bottom); SelectObject(d.hDC, line); DeleteObject(marker); MoveToEx(d.hDC, x, y, nullptr);
                    }
                }
                previous = {x, y};
            }
            SelectObject(d.hDC, old); DeleteObject(line);
        }
        SelectObject(d.hDC, font);
    } else if (d.CtlType == ODT_STATIC && d.CtlID >= IDC_DRV_TEST_LIGHT && d.CtlID <= IDC_DRV_HVCI_LIGHT) {
        bool known = d.CtlID == IDC_DRV_SECURE_LIGHT ? m_signing.secureBootKnown : m_signing.queried;
        bool pass = d.CtlID == IDC_DRV_TEST_LIGHT ? m_signing.testSigning :
            d.CtlID == IDC_DRV_SECURE_LIGHT ? !m_signing.secureBoot : !m_signing.hvci;
        bool pending = d.CtlID == IDC_DRV_HVCI_LIGHT && (!m_signing.hvciConfiguredKnown || m_signing.hvciConfigured != m_signing.hvci);
        COLORREF color = !known || pending ? RGB(171, 106, 10) :
            m_driverTrust.trusted && m_driverTrust.microsoftDriver ? m_accentColor : pass ? RGB(23, 125, 71) : RGB(191, 47, 47);
        if (m_highContrast) color = m_textColor;
        FillRect(d.hDC, &r, m_surface);
        int size = MulDiv(9, dpi, 96), gap = MulDiv(7, dpi, 96), y = (r.top + r.bottom - size) / 2;
        HBRUSH brush = CreateSolidBrush(color);
        HGDIOBJ old = SelectObject(d.hDC, brush), pen = SelectObject(d.hDC, GetStockObject(NULL_PEN));
        Ellipse(d.hDC, r.left, y, r.left + size, y + size);
        SelectObject(d.hDC, old); SelectObject(d.hDC, pen); DeleteObject(brush);
        r.left += size + gap;
        wchar_t text[128]{}; GetWindowTextW(d.hwndItem, text, _countof(text));
        SetBkMode(d.hDC, TRANSPARENT); SetTextColor(d.hDC, m_textColor);
        HGDIOBJ font = SelectObject(d.hDC, m_bodyFont);
        DrawTextW(d.hDC, text, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
        SelectObject(d.hDC, font);
    } else if (d.CtlType == ODT_TAB) {
        FillRect(d.hDC, &r, m_surface);
        bool active = TabPage((int)d.itemID) == m_cur;
        if (active) {
            r.left += 2; r.right -= 2; r.top += 2; r.bottom -= 2;
            HBRUSH b = CreateSolidBrush(m_highContrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(230, 238, 255));
            HGDIOBJ old = SelectObject(d.hDC, b), pen = SelectObject(d.hDC, GetStockObject(NULL_PEN));
            RoundRect(d.hDC, r.left, r.top, r.right, r.bottom, MulDiv(8, dpi, 96), MulDiv(8, dpi, 96));
            SelectObject(d.hDC, old); SelectObject(d.hDC, pen); DeleteObject(b);
        }
        SetBkMode(d.hDC, TRANSPARENT);
        SetTextColor(d.hDC, active ? (m_highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : m_accentColor) : m_mutedColor);
        HGDIOBJ font = SelectObject(d.hDC, SendMessageW(m_tab, WM_GETFONT, 0, 0) ? (HFONT)SendMessageW(m_tab, WM_GETFONT, 0, 0) : m_bodyFont);
        if (TabPage((int)d.itemID) >= 0) DrawTextW(d.hDC, kPages[TabPage((int)d.itemID)].title, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(d.hDC, font);
        if (d.itemState & ODS_FOCUS) { InflateRect(&r, -3, -3); DrawFocusRect(d.hDC, &r); }
    } else if (d.CtlID == IDC_APPLY) {
        FillRect(d.hDC, &r, m_background);
        COLORREF fill = disabled ? (m_highContrast ? GetSysColor(COLOR_BTNFACE) : RGB(222, 230, 240)) : selected ? RGB(23, 59, 160) : m_accentColor;
        HBRUSH brush = CreateSolidBrush(fill);
        HGDIOBJ old = SelectObject(d.hDC, brush), pen = SelectObject(d.hDC, GetStockObject(NULL_PEN));
        RoundRect(d.hDC, r.left, r.top, r.right, r.bottom, MulDiv(10, dpi, 96), MulDiv(10, dpi, 96));
        SelectObject(d.hDC, old); SelectObject(d.hDC, pen); DeleteObject(brush);
        SetBkMode(d.hDC, TRANSPARENT);
        SetTextColor(d.hDC, disabled ? m_mutedColor : m_highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(255, 255, 255));
        HGDIOBJ font = SelectObject(d.hDC, m_boldFont);
        DrawTextW(d.hDC, L"Apply changes", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(d.hDC, font);
        if (d.itemState & ODS_FOCUS) { InflateRect(&r, -4, -4); DrawFocusRect(d.hDC, &r); }
    }
}

LRESULT Panel::DrawDevices(NMLVCUSTOMDRAW &d)
{
    if (d.nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
    if (d.nmcd.dwDrawStage != CDDS_ITEMPREPAINT) return CDRF_DODEFAULT;
    const UINT dpi = GetDpiForWindow(m_hwnd);
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    RECT r; ListView_GetItemRect(m_devList, (int)d.nmcd.dwItemSpec, &r, LVIR_BOUNDS);
    RECT client; GetClientRect(m_devList, &client); r.right = client.right - px(4);
    FillRect(d.nmcd.hdc, &r, m_background);
    InflateRect(&r, -px(4), -px(5));
    bool selected = (ListView_GetItemState(m_devList, (int)d.nmcd.dwItemSpec, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    HBRUSH brush = CreateSolidBrush(selected ? (m_highContrast ? GetSysColor(COLOR_HIGHLIGHT) : RGB(230, 238, 255)) : m_surfaceColor);
    HPEN pen = CreatePen(PS_SOLID, px(1), selected ? m_accentColor : m_highContrast ? m_textColor : RGB(223, 230, 240));
    HGDIOBJ oldBrush = SelectObject(d.nmcd.hdc, brush), oldPen = SelectObject(d.nmcd.hdc, pen);
    RoundRect(d.nmcd.hdc, r.left, r.top, r.right, r.bottom, px(12), px(12));
    SelectObject(d.nmcd.hdc, oldBrush); SelectObject(d.nmcd.hdc, oldPen); DeleteObject(brush); DeleteObject(pen);
    SetBkMode(d.nmcd.hdc, TRANSPARENT);
    wchar_t name[1024], driver[512], state[512];
    int row = (int)d.nmcd.dwItemSpec;
    ListView_GetItemText(m_devList, row, 0, name, _countof(name));
    ListView_GetItemText(m_devList, row, 1, driver, _countof(driver));
    ListView_GetItemText(m_devList, row, 2, state, _countof(state));
    RECT text = {r.left + px(12), r.top + px(10), r.right - px(12), r.top + px(44)};
    COLORREF primary = selected && m_highContrast ? GetSysColor(COLOR_HIGHLIGHTTEXT) : m_textColor;
    SetTextColor(d.nmcd.hdc, primary);
    HGDIOBJ font = SelectObject(d.nmcd.hdc, m_boldFont);
    DrawTextW(d.nmcd.hdc, name, -1, &text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(d.nmcd.hdc, m_bodyFont);
    SetTextColor(d.nmcd.hdc, selected && m_highContrast ? primary : m_mutedColor);
    text.top = r.top + px(46); text.bottom = text.top + px(20);
    DrawTextW(d.nmcd.hdc, driver, -1, &text, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SetTextColor(d.nmcd.hdc, selected && m_highContrast ? primary : m_accentColor);
    text.top = r.top + px(70); text.bottom = r.bottom - px(6);
    DrawTextW(d.nmcd.hdc, state, -1, &text, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(d.nmcd.hdc, font);
    if (selected && GetFocus() == m_devList) { InflateRect(&r, -px(3), -px(3)); DrawFocusRect(d.nmcd.hdc, &r); }
    return CDRF_SKIPDEFAULT;
}

/* Keep native tab selection, keyboard input and accessibility; replace only
 * the legacy bevel painting with a flat navigation strip and white surface. */
LRESULT CALLBACK Panel::TabProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data)
{
    Panel *p = (Panel *)data;
    if (msg == WM_ERASEBKGND && p->m_surface) return 1;
    if (msg == WM_PAINT && p->m_surface) {
        PAINTSTRUCT paint; HDC dc = BeginPaint(hwnd, &paint);
        RECT client; GetClientRect(hwnd, &client);
        HDC memory = CreateCompatibleDC(dc);
        HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
        HGDIOBJ old = SelectObject(memory, bitmap);
        FillRect(memory, &client, p->m_surface);
        for (int i = 0; i < TabCtrl_GetItemCount(hwnd); ++i) {
            DRAWITEMSTRUCT item = {}; item.CtlType = ODT_TAB; item.CtlID = IDC_TAB;
            item.hDC = memory; item.hwndItem = hwnd; item.itemID = i;
            if (!TabCtrl_GetItemRect(hwnd, i, &item.rcItem)) continue;
            if (GetFocus() == hwnd && TabCtrl_GetCurFocus(hwnd) == i) item.itemState = ODS_FOCUS;
            p->DrawControl(item);
        }
        BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
        SelectObject(memory, old); DeleteObject(bitmap); DeleteDC(memory);
        EndPaint(hwnd, &paint); return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(hwnd, TabProc, 1);
    return DefSubclassProc(hwnd, msg, wp, lp);
}

bool Panel::Create(bool demo)
{
    m_demo = demo;
    g_panel = this;
    m_hwnd = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(IDD_MAIN), NULL, MainProc, (LPARAM)this);
    return m_hwnd != NULL;
}

int Panel::Run()
{
    ShowWindow(m_hwnd, SW_SHOW);
    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(m_hwnd, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    return (int)msg.wParam;
}

INT_PTR CALLBACK Panel::MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Panel *p = g_panel;
    switch (msg) {
    case WM_INITDIALOG:
        p->m_hwnd = hwnd;
        p->OnInit();
        return TRUE;
    case WM_COMMAND:
        if (p->m_inPrompt) return TRUE;
        switch (LOWORD(wp)) {
        case IDOK: if (p->m_dirty && p->m_s.open) p->Apply(); return TRUE;
        case IDC_REFRESH: p->m_eventRetry.Reset(); p->RefreshDevices(); return TRUE;
        case IDC_APPLY: p->Apply(); return TRUE;
        case IDC_REVERT: p->Revert(); return TRUE;
        case IDC_DEFAULTS: p->Defaults(); return TRUE;
        case IDC_IMPORT: p->ImportConfig(); return TRUE;
        case IDC_EXPORT: p->ExportConfig(); return TRUE;
        case IDC_ABOUT: { ModalScope modal(*p); DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ABOUT), hwnd, AboutProc, 0); return TRUE; }
        case IDCANCEL: p->OnClose(); return TRUE;
        }
        break;
    case WM_NOTIFY: {
        NMHDR *nm = (NMHDR *)lp;
        if (nm->idFrom == IDC_DEVICES && nm->code == NM_CUSTOMDRAW) {
            SetWindowLongPtrW(hwnd, DWLP_MSGRESULT, p->DrawDevices(*(NMLVCUSTOMDRAW *)lp)); return TRUE;
        }
        if (nm->idFrom == IDC_DEVICES && nm->code == LVN_GETINFOTIP) {
            NMLVGETINFOTIPW *tip = (NMLVGETINFOTIPW *)lp;
            wchar_t name[1024], driver[512], state[512];
            ListView_GetItemText(p->m_devList, tip->iItem, 0, name, _countof(name));
            ListView_GetItemText(p->m_devList, tip->iItem, 1, driver, _countof(driver));
            ListView_GetItemText(p->m_devList, tip->iItem, 2, state, _countof(state));
            std::wstring full = std::wstring(name) + L"\r\n" + driver + L"\r\n" + state;
            wcsncpy_s(tip->pszText, tip->cchTextMax, full.c_str(), _TRUNCATE); return TRUE;
        }
        if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) { p->SelectPage(p->TabPage(TabCtrl_GetCurSel(p->m_tab))); return TRUE; }
        if (nm->idFrom == IDC_DEVICES && nm->code == LVN_ITEMCHANGED) {
            NMLISTVIEW *lv = (NMLISTVIEW *)lp;
            if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED) && !(lv->uOldState & LVIS_SELECTED)) p->SelectDevice(lv->iItem);
            return TRUE;
        }
        break;
    }
    case WM_APP_DRIVER_EVENT:
        if ((unsigned)lp == p->m_sessionGen) {
            if (p->m_inPrompt) p->m_pendingEvents |= (ULONG)wp;
            else p->OnDriverEvent((ULONG)wp);
        }
        return TRUE;
    case WM_APP_DEVICE_GONE: {
        if ((unsigned)lp != p->m_sessionGen) return TRUE;       /* stale message from a session that is already closed */
        if (p->m_inPrompt) { p->m_pendingGone = (DWORD)wp; return TRUE; }
        /* refresh once; if the instance keeps failing right after reopening, stop retrying */
        std::wstring instance = p->m_sel >= 0 && p->m_sel < (int)p->m_devices.size() ? p->m_devices[p->m_sel].node.instanceId : L"";
        bool storm = p->m_eventRetry.ShouldPause(instance, GetTickCount64());
        if (storm) { p->StopEventThread(); p->SetStatusText(L"The driver stopped answering event requests: " + pn::Win32ErrorText((DWORD)wp) + L". Status updates are paused; press Refresh to retry."); }
        else { p->SetStatusText(L"The headset's driver instance went away; refreshing the device list."); p->RefreshDevices(); }
        return TRUE;
    }
    case WM_APP_DEFERRED:
        if ((unsigned)lp != p->m_sessionGen || p->m_inPrompt) return TRUE;
        if (p->m_refreshPending) { p->m_refreshPending = false; p->RefreshDevices(); }
        else if (p->m_pendingGone) {
            DWORD error = p->m_pendingGone; p->m_pendingGone = ERROR_SUCCESS;
            PostMessageW(hwnd, WM_APP_DEVICE_GONE, error, p->m_sessionGen);
        } else {
            ULONG events = p->m_pendingEvents; p->m_pendingEvents = 0;
            if (events) p->OnDriverEvent(events);
        }
        return TRUE;
    case WM_TIMER:
        if (p->m_inPrompt) return TRUE;
        if (wp == TIMER_HEALTH) { p->FinishAudioCheck(); return TRUE; }
        if (wp == TIMER_STATS && !p->m_inPrompt && p->m_sel >= 0 && p->m_sel < (int)p->m_devices.size()) {
            if (p->m_adjustingVolume && GetCapture() != GetDlgItem(p->m_page[PG_WINDOWS], IDC_WIN_VOLUME)) p->m_adjustingVolume = false;
            if (!p->m_demo && GetTickCount64() - p->m_driverRefreshTick >= 5000) {
                p->m_driverRefreshTick = GetTickCount64();
                pn::A2dpNode node;
                const auto &old = p->m_devices[p->m_sel].node;
                if (pn::GetA2dpNode(old.instanceId, node) &&
                    (_wcsicmp(old.service.c_str(), node.service.c_str()) || _wcsicmp(old.driverInf.c_str(), node.driverInf.c_str()))) {
                    p->RefreshDevices(); return TRUE;
                }
            }
            if (p->WindowsMode() && !p->m_adjustingVolume) p->LoadWindowsAudio();
        }
        if (wp == TIMER_STATS && p->m_cur == PG_ADAPTER && !p->m_inPrompt) p->LoadAdapter();
        if (wp == TIMER_STATS && p->m_cur == PG_DRIVER && !p->m_inPrompt) p->LoadReadiness();
        if (wp == TIMER_STATS) { if (p->m_s.open) { if (p->m_s.demo) p->m_s.DemoTick(); p->m_s.Refresh(false); p->UpdateOutputSummary(); if (p->m_cur == PG_STATUS) p->LoadStatus(); if (p->m_cur == PG_CODECS) p->UpdateCodecLive(); if (p->m_cur >= PG_SBC && p->m_cur <= PG_LDAC) p->UpdateCodecDetails(); } p->LoadQuality(true); return TRUE; }
        break;
    case WM_SIZE: if (wp != SIZE_MINIMIZED) p->LayoutShell(); return TRUE;
    case WM_GETMINMAXINFO: {
        UINT dpi = GetDpiForWindow(hwnd);
        RECT r = {0, 0, MulDiv(1040, dpi, 96), MulDiv(740, dpi, 96)};
        AdjustWindowRectExForDpi(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE), dpi);
        MINMAXINFO *limits = (MINMAXINFO *)lp;
        limits->ptMinTrackSize = {r.right - r.left, r.bottom - r.top}; return TRUE;
    }
    case WM_DPICHANGED: PostMessageW(hwnd, WM_APP_LAYOUT, 0, 0); return FALSE;
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: PostMessageW(hwnd, WM_APP_LAYOUT, 1, 0); return FALSE;
    case WM_APP_LAYOUT:
        if (p->m_styleDpi) { p->InitAppearance(); p->LayoutShell(); }
        return TRUE;
    case WM_DRAWITEM: if (p->m_styleDpi) { p->DrawControl(*(DRAWITEMSTRUCT *)lp); return TRUE; } break;
    case WM_CTLCOLORDLG: return (INT_PTR)p->m_background;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return p->ControlColor(msg, (HDC)wp, (HWND)lp);
    case WM_CLOSE: p->OnClose(); return TRUE;
    case WM_DESTROY: PostQuitMessage(0); return TRUE;
    }
    return FALSE;
}

INT_PTR CALLBACK Panel::AboutProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM)
{
    if (msg == WM_INITDIALOG) {
        SetText(hwnd, IDC_ABOUT_TEXT,
            L"PN-A2DP Control Panel " PN_PANEL_VERSION_TEXT L"\r\n\r\nOpen Bluetooth A2DP source driver for Windows 10 and 11 with selectable codecs: SBC (incl. XQ), AAC, aptX, aptX HD, aptX Low Latency and LDAC.\r\n\r\n"
            L"MIT License. Third-party encoders keep their own licenses (libldac Apache-2.0, libfreeaptx LGPL-2.1+, FDK AAC). \"aptX\" and \"LDAC\" are trademarks of Qualcomm and Sony; they identify the codec formats only.\r\n\r\n"
            L"https://github.com/patnawa/PN-A2DP");
        return TRUE;
    }
    if (msg == WM_COMMAND && (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL)) { EndDialog(hwnd, 0); return TRUE; }
    return FALSE;
}

void Panel::OnInit()
{
    SetText(m_hwnd, IDC_BRAND_VERSION, L"v" PN_PANEL_VERSION_TEXT);
    HICON big = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, 0);
    HICON smallIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, 0);
    SendMessageW(m_hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
    SendMessageW(m_hwnd, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
    if (m_demo) SetWindowTextW(m_hwnd, L"PN-A2DP Control Panel (demo mode)");

    m_tab = GetDlgItem(m_hwnd, IDC_TAB);
    SetWindowSubclass(m_tab, TabProc, 1, (DWORD_PTR)this);
    m_devList = GetDlgItem(m_hwnd, IDC_DEVICES);
    ListView_SetExtendedListViewStyle(m_devList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
    RECT lrc; GetClientRect(m_devList, &lrc);
    int lw = lrc.right - lrc.left - GetSystemMetrics(SM_CXVSCROLL);
    ListViewAddColumn(m_devList, 0, L"Device", lw - 172);
    ListViewAddColumn(m_devList, 1, L"Driver", 0);
    ListViewAddColumn(m_devList, 2, L"State / codec", 0);

    for (int i = 0; i < PG_COUNT; i++) {
        TCITEMW ti = {}; ti.mask = TCIF_TEXT; ti.pszText = (LPWSTR)kPages[i].title;
        TabCtrl_InsertItem(m_tab, i, &ti);
        m_tabs.push_back(i);
        m_page[i] = CreateDialogParamW(g_inst, MAKEINTRESOURCEW(kPages[i].dlg), m_hwnd, PageProc, (LPARAM)i);
    }
    CapturePageLayouts();
    SendDlgItemMessageW(m_page[PG_WINDOWS], IDC_WIN_VOLUME, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
    SendDlgItemMessageW(m_page[PG_WINDOWS], IDC_WIN_VOLUME, TBM_SETPAGESIZE, 0, 5);

    /* one-time control setup */
    HWND lv = GetDlgItem(m_page[PG_CODECS], IDC_CODEC_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_CHECKBOXES | LVS_EX_DOUBLEBUFFER);
    RECT codecRect; GetClientRect(lv, &codecRect);
    int codecWidth = codecRect.right - codecRect.left - 4;
    ListViewAddColumn(lv, 0, L"Order", codecWidth * 8 / 100);
    ListViewAddColumn(lv, 1, L"Codec", codecWidth * 19 / 100);
    ListViewAddColumn(lv, 2, L"Receiver", codecWidth * 17 / 100);
    ListViewAddColumn(lv, 3, L"Live state", codecWidth * 17 / 100);
    ListViewAddColumn(lv, 4, L"Saved settings", codecWidth * 39 / 100);
    HWND cb = GetDlgItem(m_page[PG_CODECS], IDC_PRESET);
    ComboBox_AddString(cb, L"Choose a preset to load...");
    for (int i = 0; i <= (int)pn::Preset::SbcXq; i++) ComboBox_AddString(cb, pn::PresetName((pn::Preset)i));
    ComboBox_SetCurSel(cb, 0);

    HWND sl = GetDlgItem(m_page[PG_SBC], IDC_SBC_MAX_SLIDER);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(2, 250));
    SendMessageW(sl, TBM_SETTICFREQ, 16, 0);
    SendMessageW(GetDlgItem(m_page[PG_SBC], IDC_SBC_MAX_SPIN), UDM_SETRANGE32, 2, 250);
    SendMessageW(GetDlgItem(m_page[PG_SBC], IDC_SBC_MIN_SPIN), UDM_SETRANGE32, 2, 250);

    cb = GetDlgItem(m_page[PG_AAC], IDC_AAC_OBJECT);
    ComboBox_AddString(cb, L"Automatic (AAC-LC)"); ComboBox_AddString(cb, L"AAC-LC");

    SendMessageW(GetDlgItem(m_page[PG_APTX], IDC_APTX_ID(2, IDC_APTX_LLSPIN)), UDM_SETRANGE32, 0, 255);

    sl = GetDlgItem(m_page[PG_TRANSPORT], IDC_TR_TARGET_SLIDER);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(20, 400)); SendMessageW(sl, TBM_SETTICFREQ, 20, 0);
    sl = GetDlgItem(m_page[PG_TRANSPORT], IDC_TR_MAX_SLIDER);
    SendMessageW(sl, TBM_SETRANGE, TRUE, MAKELPARAM(40, 1000)); SendMessageW(sl, TBM_SETTICFREQ, 50, 0);
    SendMessageW(GetDlgItem(m_page[PG_TRANSPORT], IDC_TR_VOLUME_SPIN), UDM_SETRANGE32, (WPARAM)(INT_PTR)-96, 0);

    lv = GetDlgItem(m_page[PG_SINK], IDC_SINK_LIST);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListViewAddColumn(lv, 0, L"SEID", 38); ListViewAddColumn(lv, 1, L"Codec", 96); ListViewAddColumn(lv, 2, L"Rates", 132);
    ListViewAddColumn(lv, 3, L"Channels", 140); ListViewAddColumn(lv, 4, L"Flags", 170);

    lv = GetDlgItem(m_page[PG_DRIVER], IDC_DRV_PLUGINS);
    ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListViewAddColumn(lv, 0, L"Plugin", 150); ListViewAddColumn(lv, 1, L"Version", 60); ListViewAddColumn(lv, 2, L"License", 100); ListViewAddColumn(lv, 3, L"Codecs", 220);

    /* explanatory notes (too long for the resource compiler's 256 character limit) */
    SetText(m_page[PG_SBC], IDC_SBC_NOTE,
        L"Leave a group empty to accept whatever the headset supports. The driver intersects your selection with the headset's capabilities and the sample rate of the Windows audio engine "
        L"(44.1 kHz for most players, 48 kHz for games and calls). Bitpool changes are applied in band while streaming; everything else triggers a renegotiation.");
    SetText(m_page[PG_AAC], IDC_AAC_NOTE,
        L"AAC-LC is negotiated using the headset's MPEG-2 or MPEG-4 capability. The selected bitrate is limited by the headset's advertised maximum. "
        L"AAC needs the codec host service with the AAC plugin (Fraunhofer FDK).");
    SetText(m_page[PG_APTX], IDC_APTX_NOTE,
        L"Leave a group empty to accept whatever the headset supports. aptX, aptX HD and aptX LL need the codec host service with the aptX plugin (libfreeaptx). "
        L"\"aptX\" is a trademark of Qualcomm; this driver implements the codec format and is not certified by Qualcomm.");
    SetText(m_page[PG_LDAC], IDC_LDAC_NOTE,
        L"The first figure applies at 48 and 96 kHz, the second at 44.1 and 88.2 kHz. High quality needs a clean 2.4 GHz environment; with ABR on, the driver drops to standard and mobile "
        L"quality in band (no reconnect) when packets start to queue and climbs back after a quiet period. 88.2 / 96 kHz are only used when the Windows audio engine runs at that rate "
        L"(Sound settings > Advanced) or when \"Expose all sample rates\" is on for exclusive mode players. LDAC needs the codec host service with the LDAC plugin (Sony libldac, Apache-2.0).");

    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_USE_PNA2DP), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_USE_INBOX), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_STAGE), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_UNSTAGE), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_SVC_START), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_SVC_STOP), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_SVC_INSTALL), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_SVC_REMOVE), !pn::IsElevated());
    Button_SetElevationRequiredState(GetDlgItem(m_page[PG_DRIVER], IDC_DRV_BOOT_MODE), !pn::IsElevated());

    SelectPage(PG_STATUS);
    RefreshDevices(false);
    SetDirty(false);
    SetTimer(m_hwnd, TIMER_STATS, 1000, NULL);
    SetTimer(m_hwnd, TIMER_HEALTH, 150, NULL);
    InitAppearance();
    UINT dpi = GetDpiForWindow(m_hwnd);
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
    int width = min(MulDiv(1200, dpi, 96), monitor.rcWork.right - monitor.rcWork.left - 32);
    int height = min(MulDiv(840, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top - 32);
    SetWindowPos(m_hwnd, NULL, monitor.rcWork.left + (monitor.rcWork.right - monitor.rcWork.left - width) / 2,
        monitor.rcWork.top + (monitor.rcWork.bottom - monitor.rcWork.top - height) / 2, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutShell();
}

void Panel::OnClose()
{
    if (m_inPrompt) return;
    if (m_dirty && m_s.open) {
        int r = Prompt(L"Apply the changed settings before closing?", MB_ICONQUESTION | MB_YESNOCANCEL);
        if (r == IDCANCEL) return;
        if (r == IDYES && !Apply()) return;
    }
    KillTimer(m_hwnd, TIMER_STATS);
    KillTimer(m_hwnd, TIMER_HEALTH);
    CloseSession();
    DestroyWindow(m_hwnd);
}

void Panel::LayoutPages()
{
    RECT rc;
    GetClientRect(m_tab, &rc);
    TabCtrl_AdjustRect(m_tab, FALSE, &rc);
    MapWindowPoints(m_tab, m_hwnd, (POINT *)&rc, 2);
    const UINT dpi = GetDpiForWindow(m_hwnd);
    const int inset = MulDiv(10, dpi, 96), top = MulDiv(8, dpi, 96);
    int width = rc.right - rc.left - inset * 2, height = rc.bottom - rc.top - top - inset;
    bool guard = m_loading; m_loading = true;
    for (int i = 0; i < PG_COUNT; i++) {
        if (!m_pageBase[i].cx || !m_pageBase[i].cy) continue;
        SetWindowPos(m_page[i], HWND_TOP, rc.left + inset, rc.top + top, width, height, SWP_NOACTIVATE);
        HDWP batch = BeginDeferWindowPos((int)m_controls[i].size());
        for (const auto &c : m_controls[i]) {
            const RECT &r = c.rect;
            batch = DeferWindowPos(batch, c.window, NULL, MulDiv(r.left, width, m_pageBase[i].cx), MulDiv(r.top, height, m_pageBase[i].cy),
                MulDiv(r.right - r.left, width, m_pageBase[i].cx), MulDiv(r.bottom - r.top, height, m_pageBase[i].cy), SWP_NOZORDER | SWP_NOACTIVATE);
        }
        if (batch) EndDeferWindowPos(batch);
    }
    HWND codecs = GetDlgItem(m_page[PG_CODECS], IDC_CODEC_LIST);
    RECT list; GetClientRect(codecs, &list);
    const int shares[] = {8, 19, 17, 17, 39};
    for (int i = 0; i < 5; ++i) ListView_SetColumnWidth(codecs, i, (list.right - MulDiv(4, dpi, 96)) * shares[i] / 100);
    m_loading = guard;
}

bool Panel::WindowsMode() const
{
    return m_sel >= 0 && m_sel < (int)m_devices.size() && !m_devices[m_sel].node.usesPnA2dp();
}

void Panel::ConfigureTabs()
{
    m_tabs.clear();
    m_tabs.push_back(PG_QUALITY);
    if (WindowsMode()) { m_tabs.push_back(PG_WINDOWS); m_tabs.push_back(PG_DRIVER); }
    else for (int i = 0; i < PG_WINDOWS; ++i) m_tabs.push_back(i);
    TabCtrl_DeleteAllItems(m_tab);
    for (size_t i = 0; i < m_tabs.size(); ++i) {
        TCITEMW item = {}; item.mask = TCIF_TEXT; item.pszText = (LPWSTR)kPages[m_tabs[i]].title;
        TabCtrl_InsertItem(m_tab, (int)i, &item);
    }
    for (int id : {IDC_APPLY, IDC_REVERT, IDC_DEFAULTS, IDC_IMPORT, IDC_EXPORT})
        ShowWindow(GetDlgItem(m_hwnd, id), WindowsMode() ? SW_HIDE : SW_SHOW);
    // Start each selected device in its actual mode, rather than leave a hidden codec page selected.
    SelectPage(PG_QUALITY);
    LayoutPages();
}

void Panel::LoadWindowsAudio()
{
    if (!WindowsMode() || m_audioBusy) return;
    const auto node = m_devices[m_sel].node;
    const unsigned generation = m_sessionGen, revision = m_audioRevision;
    struct BusyScope { bool &busy; BusyScope(bool &b) : busy(b) { busy = true; } ~BusyScope() { busy = false; } } busy(m_audioBusy);
    pn::WindowsAudioInfo snapshot;
    if (!m_demo) snapshot = pn::QueryWindowsAudio(node.instanceId);
    else snapshot.detail = L"Demo: no physical Windows output queried.";
    // COM can dispatch a click or device-selection message during discovery.
    // Never drop that user action or overwrite it with the older query result.
    if (generation != m_sessionGen || revision != m_audioRevision || !WindowsMode() || m_devices[m_sel].node.instanceId != node.instanceId) return;
    m_audio = snapshot;
    bool guard = m_loading; m_loading = true;
    HWND d = m_page[PG_WINDOWS];
    SetText(d, IDC_WIN_MODE, node.usesWindowsA2dp() ? L"Windows mode - Microsoft Bluetooth audio" : L"System audio - third-party Bluetooth driver");
    SetText(d, IDC_WIN_ENDPOINT, m_audio.endpointName.empty() ? node.name : m_audio.endpointName);
    SetText(d, IDC_WIN_STATE, WindowsPending() ? L"Restart Windows to finish switching drivers. Current output may still use the previous driver." : m_audio.detail);
    SetText(d, IDC_WIN_FORMAT, pn::WindowsAudioFormat(m_audio));
    SetText(d, IDC_WIN_DEFAULT, !m_audio.defaultKnown ? L"Unavailable" : m_audio.isDefault ? L"Yes (multimedia)" : L"No - select this output in Sound settings");
    bool volumeKnown = m_audio.active && SUCCEEDED(m_audio.volumeError);
    bool muteKnown = m_audio.active && SUCCEEDED(m_audio.muteError);
    EnableItem(d, IDC_WIN_VOLUME, volumeKnown && !m_demo);
    EnableItem(d, IDC_WIN_MUTE, muteKnown && !m_demo);
    if (volumeKnown) {
        int percent = (int)(m_audio.volume * 100.0f + 0.5f);
        SendDlgItemMessageW(d, IDC_WIN_VOLUME, TBM_SETPOS, TRUE, percent);
        SetTextF(d, IDC_WIN_LEVEL, L"%d%%", percent);
    } else { SendDlgItemMessageW(d, IDC_WIN_VOLUME, TBM_SETPOS, TRUE, 0); SetText(d, IDC_WIN_LEVEL, L"--"); }
    Check(d, IDC_WIN_MUTE, muteKnown && m_audio.muted);
    // Adapter identity is static for this selection; no throughput or RF quality is inferred.
    if (GetText(d, IDC_WIN_ADAPTER).empty()) SetText(d, IDC_WIN_ADAPTER, L"Adapter: " + (m_demo ? L"Demo" : pn::AdapterNameForDevice(node.instanceId)));
    SetText(d, IDC_WIN_DRIVER, L"Driver: " + node.driverProvider + L"  " + node.driverVersion);
    SetText(d, IDC_WIN_NOTE, L"The driver selects the Bluetooth codec and quality automatically. Codec, bitrate, SBC bitpool and packet health are not exposed here.\r\n\r\nThe format above describes the Windows audio mix, not Bluetooth transmission. Advanced codec controls require the PN-A2DP driver.");
    EnableItem(d, IDC_WIN_SOUND, !m_demo); EnableItem(d, IDC_WIN_BLUETOOTH, !m_demo);
    ListViewSet(m_devList, m_sel, 2, WindowsPending() ? L"Restart required\r\nWindows mode pending" : m_audio.active ? L"Connected\r\nCodec: managed by driver" : m_demo ? L"Demo\r\nOutput not queried" : L"Output unavailable");
    UpdateOutputSummary();
    m_loading = guard;
}

void Panel::SelectPage(int p)
{
    if (p < 0 || p >= PG_COUNT) return;
    m_cur = p;
    auto tab = std::find(m_tabs.begin(), m_tabs.end(), p);
    if (tab == m_tabs.end()) { p = WindowsMode() ? PG_WINDOWS : PG_STATUS; tab = std::find(m_tabs.begin(), m_tabs.end(), p); }
    m_cur = p;
    TabCtrl_SetCurSel(m_tab, (int)(tab - m_tabs.begin()));
    for (int i = 0; i < PG_COUNT; i++) ShowWindow(m_page[i], i == p ? SW_SHOW : SW_HIDE);
    if (p == PG_WINDOWS) LoadWindowsAudio();
    if (p == PG_STATUS) LoadStatus();
    if (p == PG_CODECS) UpdateCodecLive();
    if (p >= PG_SBC && p <= PG_LDAC) UpdateCodecDetails();
    if (p == PG_DRIVER) LoadDriver();
    if (p == PG_ADAPTER) LoadAdapter();
    if (p == PG_QUALITY) LoadQuality();
    InvalidateRect(m_tab, NULL, FALSE);
}

void Panel::SetDirty(bool d)
{
    m_dirty = d;
    EnableItem(m_hwnd, IDC_APPLY, d && m_s.open);
    EnableItem(m_hwnd, IDC_REVERT, d && m_s.open);
    UpdateCodecLive();
    UpdateCodecDetails();
    SetText(m_hwnd, IDC_SAVE_HINT, WindowsMode() ? L"Volume and mute changes take effect immediately" : !m_s.open ? L"Choose a device to configure audio" : d ? L"Unsaved changes  -  Apply to save" : L"All changes saved");
    InvalidateRect(GetDlgItem(m_hwnd, IDC_APPLY), NULL, TRUE);
    UpdateOutputSummary();
    if (d) SetStatusText(L"Settings changed; press Apply to send them to the driver.");
}

void Panel::EnableConfigPages(bool enable)
{
    for (int i = PG_CODECS; i <= PG_TRANSPORT; i++) EnableAllChildren(m_page[i], enable);
    EnableAllChildren(m_page[PG_SINK], enable);
    EnableItem(m_page[PG_TRANSPORT], IDC_TR_SWVOLUME, false); // required for endpoint volume/mute
    /* fields the driver stores but does not use yet stay greyed (see the notes on those pages) */
    static const int reserved[] = { IDC_AAC_PEAK, IDC_AAC_AFTERBURNER, 1504, 1505, 1524, 1525, 1526, 1527, 1544, 1545 };
    for (int id : reserved) { EnableItem(m_page[PG_AAC], id, false); EnableItem(m_page[PG_APTX], id, false); }
    EnableItem(m_page[PG_STATUS], IDC_ST_RESETSTATS, enable);
    EnableItem(m_page[PG_STATUS], IDC_ST_RECONNECT, enable);
    EnableItem(m_hwnd, IDC_DEFAULTS, enable);
    EnableItem(m_hwnd, IDC_IMPORT, enable);
    EnableItem(m_hwnd, IDC_EXPORT, enable);
}

/* ---- devices ------------------------------------------------------------- */

void Panel::RefreshDevices(bool keepSelection)
{
    std::wstring keep;
    if (keepSelection && m_sel >= 0 && m_sel < (int)m_devices.size()) keep = m_devices[m_sel].node.instanceId;
    if (m_inPrompt) { m_refreshPending = true; return; }
    if (m_dirty && m_s.open) {
        int r = Prompt(L"Apply the changed settings before refreshing?", MB_ICONQUESTION | MB_YESNOCANCEL);
        m_refreshPending = false;
        if (r == IDCANCEL || (r == IDYES && !Apply())) return;
    }
    CloseSession();
    m_devices.clear();
    if (m_demo) {
        DeviceEntry d;
        d.demo = true;
        d.node.name = L"WH-1000XM5 (demo)";
        d.node.instanceId = L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}_LOCALMFG&000F\\7&DEMO&0&E8EC4A123456_C00000000";
        d.node.address = 0xE8EC4A123456ull; d.node.service = PN_DRIVER_SERVICE; d.node.driverProvider = PN_INF_PROVIDER; d.node.driverVersion = L"0.1.0.0";
        d.node.driverInf = L"oem42.inf"; d.node.driverDesc = L"PN-A2DP Bluetooth Stereo (A2DP) Device"; d.node.present = true;
        d.ifacePath = L"(demo)";
        m_devices.push_back(d);
        DeviceEntry e;
        e.demo = true;
        e.node.name = L"JBL Flip 6 (demo, inbox driver)";
        e.node.instanceId = L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}_LOCALMFG&0057\\7&DEMO&0&F4B7E2A1C3D5_C00000000";
        e.node.address = 0xF4B7E2A1C3D5ull; e.node.service = L"BthA2dp"; e.node.driverProvider = L"Microsoft"; e.node.driverVersion = L"10.0.26100.1";
        e.node.driverInf = PN_INBOX_A2DP_INF; e.node.driverDesc = L"Microsoft Bluetooth A2dp Source"; e.node.present = false;
        m_devices.push_back(e);
    } else {
        auto nodes = pn::EnumerateA2dpSinkNodes();
        auto ifaces = pn::EnumerateControlInterfaces();
        for (auto &n : nodes) {
            DeviceEntry d; d.node = n;
            for (auto &i : ifaces) if (_wcsicmp(i.instanceId.c_str(), n.instanceId.c_str()) == 0) d.ifacePath = i.path;
            m_devices.push_back(d);
        }
        for (auto &i : ifaces) {
            bool found = false;
            for (auto &d : m_devices) if (_wcsicmp(d.node.instanceId.c_str(), i.instanceId.c_str()) == 0) found = true;
            if (!found) { DeviceEntry d; d.node.instanceId = i.instanceId; d.node.name = L"PN-A2DP device"; d.node.service = PN_DRIVER_SERVICE; d.ifacePath = i.path; d.node.present = true; m_devices.push_back(d); }
        }
    }
    ListView_DeleteAllItems(m_devList);
    int reselect = -1;
    for (size_t i = 0; i < m_devices.size(); i++) {
        DeviceEntry &d = m_devices[i];
        std::wstring name = d.node.name.empty() ? pn::BtAddressText(d.node.address) : d.node.name;
        int row = ListViewAddRow(m_devList, name, (LPARAM)i);
        ListViewSet(m_devList, row, 1, d.node.usesPnA2dp() ? L"PN-A2DP driver mode" : d.node.usesWindowsA2dp() ? L"Windows mode" : d.node.service.empty() ? L"none" : _wcsicmp(d.node.driverInf.c_str(), PN_INBOX_A2DP_INF) == 0 ? L"Microsoft" : d.node.driverProvider.empty() ? L"other" : d.node.driverProvider);
        std::wstring state = !d.node.present ? L"not present" : d.node.connected ? L"connected" : d.node.started ? L"paired" : L"driver not started";
        if (!d.ifacePath.empty() && !d.demo) {
            pn::Device tmp;
            PNA2DP_STATUS st;
            if (tmp.Open(d.ifacePath, false) && tmp.GetStatus(st)) { state = pn::StateName(st.State); if (st.Codec) { state += L" "; state += pn::CodecName(st.Codec); } }
        } else if (d.demo && d.node.usesPnA2dp()) state = L"Streaming LDAC";
        ListViewSet(m_devList, row, 2, state);
        if (!keep.empty() && _wcsicmp(keep.c_str(), d.node.instanceId.c_str()) == 0) reselect = row;
    }
    if (reselect < 0 && !m_devices.empty()) {
        reselect = 0;
        for (size_t i = 0; i < m_devices.size(); i++) if (!m_devices[i].ifacePath.empty()) { reselect = (int)i; break; }
    }
    if (reselect >= 0) {
        ListView_SetItemState(m_devList, reselect, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    } else {
        m_sel = -1; m_s.Reset(); ConfigureTabs(); EnableConfigPages(false); LoadAll(); SetDirty(false);
        SetStatusText(L"No Bluetooth stereo device is paired. Pair a headset in Windows Settings, then press Refresh.");
    }
}

void Panel::CloseSession()
{
    StopEventThread();
    m_history.Observe(GetTickCount64(), false, {}, {}, false, {});
    m_healthValid = false;
    m_healthAction = pn::HealthAction::None;
    SetText(m_page[PG_QUALITY], IDC_HEALTH_RESULTS, L"Press Check my audio for a fresh device, output, volume and codec check.");
    SetText(m_page[PG_QUALITY], IDC_HEALTH_TIMESTAMP, L"Read-only check; no settings changed and no audio recorded.");
    m_pendingEvents = 0;
    m_pendingGone = ERROR_SUCCESS;
    m_refreshPending = false;
    m_s.dev.Close();
    m_s.open = false;
    m_s.demo = false;
    m_audio = {};
    if (m_page[PG_WINDOWS]) SetText(m_page[PG_WINDOWS], IDC_WIN_ADAPTER, L"");
    m_adjustingVolume = false;
}

void Panel::SelectDevice(int index)
{
    if (m_inPrompt) return;
    if (index < 0 || index >= (int)m_devices.size()) return;
    if (m_dirty && m_s.open && index != m_sel) {
        std::wstring target = m_devices[index].node.instanceId;
        int r = Prompt(L"Apply the changed settings to the previous headset first?", MB_ICONQUESTION | MB_YESNOCANCEL);
        if (r == IDCANCEL || (r == IDYES && !Apply())) {
            /* Keep the previous session and its unsaved edits after a rejected save. */
            m_inPrompt = true;
            ListView_SetItemState(m_devList, index, 0, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_SetItemState(m_devList, m_sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            m_inPrompt = false;
            return;
        }
        /* the device list may have been rebuilt while the prompt was up: find the target again */
        index = -1;
        for (size_t i = 0; i < m_devices.size(); i++) if (_wcsicmp(m_devices[i].node.instanceId.c_str(), target.c_str()) == 0) index = (int)i;
        if (index < 0) return;
    }
    CloseSession();
    m_sel = index;
    const DeviceEntry d = m_devices[index];
    if (_wcsicmp(m_historyId.c_str(), d.node.instanceId.c_str()) != 0) {
        m_history.Reset(); m_historyId = d.node.instanceId; m_fallbackRequestedAt = 0;
    }
    const unsigned generation = m_sessionGen;
    m_s.Reset();
    if (d.demo && d.node.usesPnA2dp()) {
        m_s.DemoFill();
    } else if (!d.ifacePath.empty() && d.node.usesPnA2dp()) {
        if (m_s.dev.Open(d.ifacePath, true) || m_s.dev.Open(d.ifacePath, false)) {
            m_s.open = true;
            m_s.dev.GetVersion(m_s.ver);
            m_s.Refresh(true);
            m_s.LoadConfig();
            StartEventThread();
        } else {
            SetStatusText(L"Cannot open the PN-A2DP control interface: " + pn::Win32ErrorText(m_s.dev.LastError()));
        }
    }
    ConfigureTabs();
    EnableConfigPages(m_s.open);
    LoadAll();
    if (generation != m_sessionGen) return;
    SetDirty(false);
    if (m_s.open) SetStatusText(L"Connected to the PN-A2DP driver instance for " + (d.node.name.empty() ? pn::BtAddressText(d.node.address) : d.node.name) + L".");
    else if (!m_s.open) {
        bool inbox = _wcsicmp(d.node.driverInf.c_str(), PN_INBOX_A2DP_INF) == 0;
        SetStatusText(WindowsPending() ? L"Restart Windows to complete the selected driver change." : inbox ? L"Windows mode: Windows manages the Bluetooth codec. Volume and mute are controlled directly."
                            : L"This headset uses another driver (" + (d.node.driverProvider.empty() ? std::wstring(L"unknown provider") : d.node.driverProvider) + L"). Switch it to PN-A2DP on the Driver tab to configure codecs.");
    }
}

void Panel::StartEventThread()
{
    if (m_s.demo || !m_s.open) return;
    m_evStop = false;
    HWND hwnd = m_hwnd;
    pn::Device *dev = &m_s.dev;
    std::atomic<bool> *stop = &m_evStop;
    LPARAM gen = (LPARAM)m_sessionGen;     /* messages from an older session are dropped by the receiver */
    m_evThread = std::thread([hwnd, dev, stop, gen]() {
        int failures = 0;
        while (!*stop) {
            ULONG mask = 0;
            DWORD e = 0;
            /* the request stays pended in the driver until an event happens or Cancel() is called */
            if (dev->WaitEvent(mask, INFINITE, &e)) { failures = 0; PostMessageW(hwnd, WM_APP_DRIVER_EVENT, mask, gen); continue; }
            if (*stop) break;
            if (e == ERROR_OPERATION_ABORTED) { Sleep(100); continue; }
            /* device gone, or a driver that does not support the event IOCTL: back off, then give up quietly */
            if (++failures < 3) { Sleep(500); continue; }
            PostMessageW(hwnd, WM_APP_DEVICE_GONE, e, gen);
            break;
        }
    });
}

void Panel::StopEventThread()
{
    m_evStop = true;
    if (m_evThread.joinable()) {
        /* a Cancel() issued before the thread re-pended its request would be lost: repeat it until the thread is gone */
        HANDLE h = (HANDLE)m_evThread.native_handle();
        for (int i = 0; i < 100; i++) {
            m_s.dev.Cancel();
            if (WaitForSingleObject(h, 50) != WAIT_TIMEOUT) break;
        }
        m_evThread.join();
    }
    m_sessionGen++;
}

void Panel::OnDriverEvent(ULONG mask)
{
    if (!m_s.open) return;
    m_s.Refresh((mask & PNA2DP_EVENT_CAPS_CHANGED) != 0);
    if (mask & PNA2DP_EVENT_HOST_CHANGED) m_s.dev.GetVersion(m_s.ver);
    if (mask & PNA2DP_EVENT_CONFIG_CHANGED) {
        PNA2DP_CONFIG fresh;
        if (m_s.dev.GetConfig(fresh) && memcmp(&fresh, &m_s.applied, sizeof(fresh)) != 0) {
            m_s.applied = fresh;
            if (!m_dirty) { m_s.config = fresh; LoadAll(); SetStatusText(L"Configuration changed by another tool; reloaded."); }
            else SetStatusText(L"The driver configuration changed elsewhere (pna2dpctl?); Revert reloads it.");
        }
    }
    if (mask & (PNA2DP_EVENT_CAPS_CHANGED | PNA2DP_EVENT_HOST_CHANGED)) { LoadSink(); LoadCodecs(); UpdateCodecAvailability(); }
    if (mask & (PNA2DP_EVENT_STATE_CHANGED | PNA2DP_EVENT_SCO_CHANGED | PNA2DP_EVENT_DELAY_REPORT | PNA2DP_EVENT_ERROR | PNA2DP_EVENT_HOST_CHANGED)) {
        LoadStatus();
        UpdateCodecLive();
        if (m_sel >= 0) {
            std::wstring state = pn::StateName(m_s.status.State);
            if (m_s.status.Codec) { state += L" "; state += pn::CodecName(m_s.status.Codec); }
            ListViewSet(m_devList, m_sel, 2, state);
            InvalidateRect(m_devList, NULL, FALSE);
        }
        if (mask & PNA2DP_EVENT_ERROR) SetStatusText(L"Driver reported an error: " + pn::NtStatusText(m_s.status.LastError));
    }
    UpdateOutputSummary();
}

/* ---- apply / revert ------------------------------------------------------ */

bool Panel::CollectConfig(PNA2DP_CONFIG &c)
{
    c = m_s.config;
    SaveCodecs(c); SaveSbc(c); SaveAac(c); SaveAptx(c); SaveLdac(c); SaveTransport(c);
    if (Checked(m_page[PG_ADAPTER], IDC_AD_AUTO)) {
        c.Flags |= PNA2DP_CFG_AUTO_QUALITY | PNA2DP_CFG_ABR_ENABLE;
        c.Ldac.AbrEnable = 1;
    } else c.Flags &= ~PNA2DP_CFG_AUTO_QUALITY;
    /* client side validation mirrors the driver's clamps so the user sees what will stick */
    if (c.TargetLatencyMs < 20) c.TargetLatencyMs = 20;
    if (c.TargetLatencyMs > 400) c.TargetLatencyMs = 400;
    if (c.MaxLatencyMs < c.TargetLatencyMs + 20) c.MaxLatencyMs = c.TargetLatencyMs + 20;
    if (c.MaxLatencyMs > 1000) c.MaxLatencyMs = 1000;
    if (c.Sbc.MaxBitpool > 250) c.Sbc.MaxBitpool = 250;
    if (c.Sbc.MaxBitpool && c.Sbc.MinBitpool > c.Sbc.MaxBitpool) c.Sbc.MinBitpool = c.Sbc.MaxBitpool;
    c.EnabledCodecMask |= PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    c.Size = sizeof(c); c.Version = PNA2DP_ABI_VERSION;
    return true;
}

bool Panel::Apply()
{
    if (!m_s.open) return false;
    PNA2DP_CONFIG c;
    CollectConfig(c);
    /* The driver updates VolumeDb16 itself on every Windows volume change without raising an event;
     * never overwrite it with our snapshot unless the user typed a new value. */
    if (!m_volumeEdited) {
        PNA2DP_CONFIG live;
        if (!m_s.demo && m_s.dev.GetConfig(live)) c.VolumeDb16 = live.VolumeDb16; else c.VolumeDb16 = m_s.applied.VolumeDb16;
    }
    if (!m_s.StoreConfig(c)) {
        Error(L"The driver rejected the configuration. Your changes have been kept; correct the problem and try Apply again.", m_s.dev.LastError());
        return false;
    }
    LoadAll();
    SetDirty(false);
    SetStatusText(L"Configuration applied and saved for this headset.");
    return true;
}

void Panel::Revert()
{
    if (!m_s.open) return;
    m_s.LoadConfig();
    LoadAll();
    SetDirty(false);
    SetStatusText(L"Reverted to the driver's current configuration.");
}

void Panel::Defaults()
{
    if (!m_s.open) return;
    LONG vol = m_s.config.VolumeDb16;
    pn::ConfigDefaults(m_s.config);
    m_s.config.VolumeDb16 = vol;
    LoadAll();
    SetDirty(true);
    SetStatusText(L"Driver defaults loaded; press Apply to use them.");
}

static bool PickFile(HWND owner, bool save, std::wstring &path)
{
    wchar_t buf[MAX_PATH] = L"pna2dp-settings.txt";
    OPENFILENAMEW ofn = { sizeof(ofn) };
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"PN-A2DP settings (*.txt)\0*.txt\0All files\0*.*\0";
    ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"txt";
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    if (ok) path = buf;
    return ok != FALSE;
}

void Panel::ExportConfig()
{
    if (!m_s.open) return;
    ModalScope modal(*this);
    PNA2DP_CONFIG c; CollectConfig(c);
    std::wstring path;
    if (!PickFile(m_hwnd, true, path)) return;
    DWORD error = pn::WriteConfigFile(path, c);
    if (error) { Error(L"Cannot export settings. The existing file has been kept.", error); return; }
    SetStatusText(L"Settings exported to " + path);
}

void Panel::ImportConfig()
{
    if (!m_s.open) return;
    ModalScope modal(*this);
    std::wstring path;
    if (!PickFile(m_hwnd, false, path)) return;
    PNA2DP_CONFIG c;
    DWORD error = pn::ReadConfigFile(path, c);
    if (error) { Error(L"Cannot import settings. Choose a valid UTF-8 PN-A2DP settings file (up to 1 MiB). Your settings have been kept.", error); return; }
    c.VolumeDb16 = m_s.config.VolumeDb16;
    m_s.config = c;
    LoadAll();
    SetDirty(true);
    SetStatusText(L"Settings imported from " + path + L"; press Apply to use them.");
}

/* ---- page loading -------------------------------------------------------- */

void Panel::LoadAll()
{
    m_loading = true;
    LoadStatus(); LoadCodecs(); LoadSbc(); LoadAac(); LoadAptx(); LoadLdac(); LoadTransport(); LoadSink(); LoadDriver();
    if (WindowsMode()) LoadWindowsAudio();
    Check(m_page[PG_ADAPTER], IDC_AD_AUTO, (m_s.config.Flags & PNA2DP_CFG_AUTO_QUALITY) != 0);
    EnableItem(m_page[PG_ADAPTER], IDC_AD_AUTO, m_s.open);
    EnableItem(m_page[PG_ADAPTER], IDC_AD_BEST, m_s.open);
    if (m_cur == PG_ADAPTER) LoadAdapter();
    UpdateCodecAvailability();
    ComboBox_SetCurSel(GetDlgItem(m_page[PG_CODECS], IDC_PRESET), 0);
    EnableItem(m_page[PG_CODECS], IDC_PRESET_APPLY, false);
    SetText(m_page[PG_CODECS], IDC_PRESET_DESC, L"Choose a preset to preview it. Load preset changes the editable settings; Apply saves them.");
    m_loading = false;
    LoadQuality();
}

void Panel::LoadQuality(bool observe)
{
    HWND d = m_page[PG_QUALITY];
    bool selected = m_sel >= 0 && m_sel < (int)m_devices.size();
    if (observe) {
        PNA2DP_QUALITY q{};
        bool qv = m_s.open && !m_demo && m_s.dev.GetQuality(q);
        m_history.Observe(GetTickCount64(), m_s.open && m_s.telemetryValid, m_s.status, m_s.stats, qv, q);
    }
    auto summary = m_history.Summary(GetTickCount64());
    if (!selected) { summary.title = L"Select a Bluetooth device"; summary.reason = L"Pair and connect a stereo receiver, then press Refresh."; }
    else if (WindowsMode()) { summary.title = m_devices[m_sel].node.usesWindowsA2dp() ? L"Windows manages Bluetooth quality" : L"System driver manages Bluetooth quality"; summary.reason = L"Codec, bitrate and packet health are unavailable in this mode. Check my audio can inspect the output, volume and device state."; summary.recent = L"Bluetooth measurements unavailable"; }
    if (m_demo) summary.title = L"Demo: " + summary.title;
    if (CanOfferFallback()) summary.reason += L" Open Events for optional SBC recovery.";
    SetText(d, IDC_QUALITY_TITLE, summary.title);
    SetText(d, IDC_QUALITY_REASON, summary.reason);
    SetText(d, IDC_QUALITY_RECENT, summary.recent);
    if (m_s.open && m_s.telemetryValid && m_s.status.Codec) SetTextF(d, IDC_QUALITY_BITRATE, L"%s: %u kbps | ceiling %u", pn::CodecName(m_s.status.Codec), m_s.status.BitrateBps / 1000, m_s.status.MaxBitrateBps / 1000);
    else SetText(d, IDC_QUALITY_BITRATE, L"Bitrate unavailable");
    EnableItem(d, IDC_HEALTH_CHECK, selected && !m_healthJob);
    EnableItem(d, IDC_HEALTH_EXPORT, m_healthValid && !m_healthJob);
    EnableItem(d, IDC_HEALTH_FIX, m_healthValid && m_healthAction != pn::HealthAction::None && !m_demo);
    EnableItem(d, IDC_QUALITY_EVENTS, selected);
    InvalidateRect(GetDlgItem(d, IDC_QUALITY_GRAPH), nullptr, FALSE);
}

bool Panel::CanOfferFallback() const
{
    ULONGLONG now = GetTickCount64();
    return !m_demo && !m_dirty && m_s.open && m_s.telemetryValid &&
        (!m_fallbackRequestedAt || now-m_fallbackRequestedAt >= 60000) &&
        pn::CanActivateCodec(m_s.caps, m_s.ver, PNA2DP_CODEC_SBC) && m_history.SuggestSbcFallback(now);
}

void Panel::ShowQualityEvents()
{
    struct View { std::wstring text; bool fallback; } view{m_history.EventText(GetTickCount64()), CanOfferFallback()};
    const unsigned generation = m_sessionGen;
    INT_PTR result = DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_QUALITY_EVENTS), m_hwnd,
        [](HWND dialog, UINT message, WPARAM wp, LPARAM lp) -> INT_PTR {
            if (message == WM_INITDIALOG) {
                auto view = (View *)lp;
                SetDlgItemTextW(dialog, IDC_EVENT_TEXT, view->text.c_str());
                EnableWindow(GetDlgItem(dialog, IDC_EVENT_FALLBACK), view->fallback);
                return TRUE;
            }
            if (message == WM_COMMAND && (LOWORD(wp) == IDCANCEL || LOWORD(wp) == IDC_EVENT_FALLBACK)) {
                EndDialog(dialog, LOWORD(wp)); return TRUE;
            }
            return FALSE;
        }, (LPARAM)&view);
    // Revalidate after the modal loop: device state can change while it is open.
    if (result != IDC_EVENT_FALLBACK || generation != m_sessionGen || !CanOfferFallback()) return;
    PNA2DP_CONFIG config = m_s.applied;
    if (!pn::PreferCodec(config, m_s.caps, m_s.ver, PNA2DP_CODEC_SBC)) return;
    config.Flags |= PNA2DP_CFG_ABR_ENABLE;
    m_fallbackRequestedAt = GetTickCount64();
    m_s.config = config; LoadAll(); SetDirty(true);
    if (Apply()) {
        m_history.RecordEvent(GetTickCount64(), L"User requested adaptive SBC fallback; saved SBC as first preference.");
        SetStatusText(L"Requested adaptive SBC. The live codec confirms receiver acceptance; your other settings are preserved.");
    } else m_history.RecordEvent(GetTickCount64(), L"SBC fallback request failed; inspect the driver status before retrying.");
}

void Panel::CheckAudio()
{
    if (m_healthJob || m_sel < 0 || m_sel >= (int)m_devices.size()) return;
    m_healthValid = false; m_healthAction = pn::HealthAction::None;
    auto job = std::make_shared<HealthJob>(); job->generation = m_sessionGen;
    m_healthJob = job;
    SetText(m_page[PG_QUALITY], IDC_HEALTH_TIMESTAMP, L"Checking this device in the background...");
    SetText(m_page[PG_QUALITY], IDC_HEALTH_RESULTS, L"Reading device, output, volume, service and codec information. You can keep using the panel.");
    if (m_demo) {
        auto &h = job->result; h.demo = h.nodeKnown = h.found = true; h.node = m_devices[m_sel].node;
        h.status = m_s.status; h.stats = m_s.stats; h.version = m_s.ver; h.caps = m_s.caps; h.config = m_s.applied;
        h.statusValid = h.statsValid = h.versionValid = h.capsValid = h.configValid = m_s.open;
        GetSystemTime(&h.capturedUtc); job->done = true;
    } else {
        auto id = m_devices[m_sel].node.instanceId;
        try {
            // The worker owns all memory and handles. Closing/changing the panel
            // cannot invalidate them; only the UI consumes a completed result.
            std::thread([job, id] {
                try { job->result = pn::CollectHealth(id); } catch (...) { job->failed = true; }
                job->done.store(true, std::memory_order_release);
            }).detach();
        } catch (...) { job->failed = true; job->done = true; }
    }
    LoadQuality();
}

void Panel::FinishAudioCheck()
{
    if (!m_healthJob || !m_healthJob->done.load(std::memory_order_acquire)) return;
    auto job = m_healthJob; m_healthJob.reset();
    if (job->generation != m_sessionGen) { LoadQuality(); return; }
    HWND d = m_page[PG_QUALITY];
    if (job->failed) { SetText(d, IDC_HEALTH_RESULTS, L"The diagnostic check could not finish. Retry the check."); SetText(d, IDC_HEALTH_TIMESTAMP, L"Check failed; no settings changed."); LoadQuality(); return; }
    m_health = std::move(job->result); m_healthValid = true;
    auto checks = pn::EvaluateHealth(m_health, m_history.Summary(GetTickCount64()));
    SetText(d, IDC_HEALTH_RESULTS, pn::HealthCheckText(checks));
    m_healthAction = pn::HealthAction::Sound;
    for (const auto &check : checks) if ((check.level == pn::HealthLevel::Error || check.level == pn::HealthLevel::Warning || check.level == pn::HealthLevel::Unknown) && check.action != pn::HealthAction::None) { m_healthAction = check.action; break; }
    const auto &t = m_health.capturedUtc;
    SetTextF(d, IDC_HEALTH_TIMESTAMP, L"%sChecked at %02u:%02u:%02u UTC. Recheck after changing settings.", m_demo ? L"Demo - " : L"", t.wHour, t.wMinute, t.wSecond);
    const wchar_t *action = m_healthAction == pn::HealthAction::Driver ? L"Open Driver requirements" : m_healthAction == pn::HealthAction::Adapter ? L"Open Adapter details" : m_healthAction == pn::HealthAction::Codecs ? L"Open codec settings" : m_healthAction == pn::HealthAction::Bluetooth ? L"Open Bluetooth settings" : L"Open Sound settings";
    SetText(d, IDC_HEALTH_FIX, action);
    LoadQuality();
}

void Panel::ExportDiagnostics()
{
    if (!m_healthValid || m_healthJob) return;
    ModalScope modal(*this);
    // Freeze the completed check and history before entering a common dialog.
    std::wstring report = pn::DiagnosticReport(m_health, m_history, GetTickCount64(), L"" PN_PANEL_VERSION_TEXT);
    wchar_t file[MAX_PATH] = L"PN-A2DP-diagnostics.txt";
    OPENFILENAMEW dialog{sizeof(dialog)}; dialog.hwndOwner = m_hwnd;
    dialog.lpstrFile = file; dialog.nMaxFile = _countof(file);
    dialog.lpstrFilter = L"Diagnostic text (*.txt)\0*.txt\0\0"; dialog.lpstrDefExt = L"txt";
    dialog.lpstrTitle = L"Export diagnostics - identifiers omitted, no audio recorded";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&dialog)) return;
    DWORD error = pn::WriteUtf8FileAtomic(file, report);
    if (error) Error(L"Cannot export diagnostics. Any existing file has been kept.", error);
    else SetStatusText(L"Diagnostic report exported. Device identifiers and audio recordings are excluded.");
}

void Panel::OpenHealthAction()
{
    if (!m_healthValid || m_demo) return;
    if (m_healthAction == pn::HealthAction::Driver) SelectPage(PG_DRIVER);
    else if (m_healthAction == pn::HealthAction::Adapter) SelectPage(PG_ADAPTER);
    else if (m_healthAction == pn::HealthAction::Codecs) SelectPage(PG_CODECS);
    else if (m_healthAction != pn::HealthAction::None) {
        if ((INT_PTR)ShellExecuteW(m_hwnd, L"open", m_healthAction == pn::HealthAction::Bluetooth ? L"ms-settings:bluetooth" : L"ms-settings:sound", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
            Prompt(L"Windows Settings could not be opened. Open Settings from the Start menu.");
    }
}

void Panel::LoadAdapter()
{
    HWND d = m_page[PG_ADAPTER];
    const DeviceEntry *entry = m_sel >= 0 && m_sel < (int)m_devices.size() ? &m_devices[m_sel] : nullptr;
    SetText(d, IDC_AD_NAME, entry && !m_demo ? pn::AdapterNameForDevice(entry->node.instanceId) : L"No physical adapter selected");
    PNA2DP_QUALITY q = {};
    if (m_demo) SetText(d, IDC_AD_REPORT, L"Demo: physical adapter health is not simulated.");
    else if (!m_s.open) SetText(d, IDC_AD_REPORT, L"Select a device using PN-A2DP to inspect its adapter and live connection health.");
    else if (!m_s.dev.GetQuality(q)) SetText(d, IDC_AD_REPORT, L"Quality telemetry unavailable. This feature requires driver 0.1.0.16 or later.\r\n" + pn::Win32ErrorText(m_s.dev.LastError()));
    else SetText(d, IDC_AD_REPORT, pn::QualityReport(q));
}

void Panel::LoadStatus()
{
    HWND d = m_page[PG_STATUS];
    bool guard = m_loading; m_loading = true;
    if (!m_s.open) {
        const int ids[] = { IDC_ST_DEVICE, IDC_ST_ADDRESS, IDC_ST_STATE, IDC_ST_CODEC, IDC_ST_FORMAT, IDC_ST_ENCODER, IDC_ST_BITRATE, IDC_ST_LATENCY, IDC_ST_SINKDELAY, IDC_ST_MTU,
                            IDC_ST_AVDTP, IDC_ST_SEID, IDC_ST_FLAGS, IDC_ST_LASTERR, IDC_ST_PCM, IDC_ST_PACKETS, IDC_ST_BYTES, IDC_ST_FRAMES, IDC_ST_DROPPED, IDC_ST_WRITEFAIL,
                            IDC_ST_TRIMS, IDC_ST_QUEUE, IDC_ST_TXTIME, IDC_ST_ABR, IDC_ST_HOSTRTT, IDC_ST_HOSTTO, IDC_ST_DRIVERVER, IDC_ST_ENCODERS };
        for (int id : ids) SetText(d, id, L"-");
        if (m_sel >= 0 && m_sel < (int)m_devices.size()) {
            SetText(d, IDC_ST_DEVICE, m_devices[m_sel].node.name);
            SetText(d, IDC_ST_ADDRESS, pn::BtAddressText(m_devices[m_sel].node.address));
            const pn::A2dpNode &n = m_devices[m_sel].node;
            bool inbox = _wcsicmp(n.driverInf.c_str(), PN_INBOX_A2DP_INF) == 0;
            SetText(d, IDC_ST_STATE, n.usesPnA2dp() ? L"PN-A2DP driver not responding" : inbox ? L"served by the Microsoft inbox driver"
                                     : L"other driver: " + (n.driverProvider.empty() ? std::wstring(L"unknown") : n.driverProvider));
        }
    } else {
        const PNA2DP_STATUS &s = m_s.status;
        const PNA2DP_STATS &t = m_s.stats;
        SetText(d, IDC_ST_DEVICE, s.RemoteName[0] ? s.RemoteName : L"(name not yet known)");
        SetText(d, IDC_ST_ADDRESS, pn::BtAddressText(s.RemoteAddress));
        SetText(d, IDC_ST_STATE, pn::StateName(s.State));
        SetTextF(d, IDC_ST_CODEC, L"%s%s", pn::CodecName(s.Codec), (s.Flags & PNA2DP_STATUS_FLAG_ABR_ACTIVE) ? L" (ABR reduced)" : L"");
        if (s.SampleRate) SetTextF(d, IDC_ST_FORMAT, L"%u Hz, %u ch, %u-bit in", s.SampleRate, s.Channels, s.BitsPerSample); else SetText(d, IDC_ST_FORMAT, L"-");
        SetText(d, IDC_ST_ENCODER, s.EncoderOwner == 1 ? L"kernel (SBC)" : s.EncoderOwner == 2 ? L"codec host plugin" : L"-");
        if (s.BitrateBps) SetTextF(d, IDC_ST_BITRATE, L"%u kbps (ceiling %u kbps)", s.BitrateBps / 1000, s.MaxBitrateBps / 1000); else SetText(d, IDC_ST_BITRATE, L"-");
        SetTextF(d, IDC_ST_LATENCY, L"%u ms estimated end to end", s.EstimatedLatencyUs / 1000);
        if (s.Flags & PNA2DP_STATUS_FLAG_DELAY_REPORTING) SetTextF(d, IDC_ST_SINKDELAY, L"%u.%u ms reported by the headset", s.SinkReportedDelayUs / 1000, (s.SinkReportedDelayUs / 100) % 10);
        else SetText(d, IDC_ST_SINKDELAY, L"no delay reporting");
        SetTextF(d, IDC_ST_MTU, L"%u bytes", s.MediaMtu);
        SetTextF(d, IDC_ST_AVDTP, L"%x.%x", s.AvdtpVersion >> 8, s.AvdtpVersion & 0xFF);
        SetTextF(d, IDC_ST_SEID, L"%u", s.RemoteSeid);
        SetText(d, IDC_ST_FLAGS, pn::StatusFlagsText(s.Flags));
        SetText(d, IDC_ST_LASTERR, s.LastError ? pn::NtStatusText(s.LastError) : L"none");
        SetText(d, IDC_ST_PCM, Thousands(t.PcmFramesConsumed));
        SetText(d, IDC_ST_PACKETS, Thousands(t.PacketsSent));
        SetText(d, IDC_ST_BYTES, Thousands(t.BytesSent));
        SetText(d, IDC_ST_FRAMES, Thousands(t.EncodedFrames));
        SetText(d, IDC_ST_DROPPED, Thousands(t.PacketsDropped));
        SetText(d, IDC_ST_WRITEFAIL, Thousands(t.WriteFailures));
        SetText(d, IDC_ST_TRIMS, Thousands(t.Underruns));
        SetTextF(d, IDC_ST_QUEUE, L"%u now, %u peak", t.QueueDepthPackets, t.QueueDepthMaxPackets);
        SetTextF(d, IDC_ST_TXTIME, L"%u us avg, %u us max", t.TxCompletionAvgUs, t.TxCompletionMaxUs);
        SetTextF(d, IDC_ST_ABR, L"level %u, %u down, %u up", t.AbrLevel, t.AbrDowngrades, t.AbrUpgrades);
        SetTextF(d, IDC_ST_HOSTRTT, L"%u us", t.HostRoundTripAvgUs);
        SetText(d, IDC_ST_HOSTTO, Thousands(t.HostTimeouts));
        SetTextF(d, IDC_ST_DRIVERVER, L"PN-A2DP %u.%u.%u (ABI %u)", m_s.ver.DriverVersion >> 24, (m_s.ver.DriverVersion >> 16) & 0xFF, m_s.ver.DriverVersion & 0xFFFF, m_s.ver.AbiVersion);
        std::wstring enc = L"kernel:";
        for (ULONG c = 1; c < PNA2DP_CODEC_MAX; c++) if (m_s.ver.KernelCodecMask & PNA2DP_CODEC_BIT(c)) { enc += L" "; enc += pn::CodecName(c); }
        enc += L"   codec host:";
        bool any = false;
        for (ULONG c = 1; c < PNA2DP_CODEC_MAX; c++) if (m_s.ver.HostCodecMask & PNA2DP_CODEC_BIT(c)) { enc += L" "; enc += pn::CodecName(c); any = true; }
        if (!any) enc += L" (none registered)";
        SetText(d, IDC_ST_ENCODERS, enc);
    }
    pn::ServiceInfo si;
    if (m_s.demo) SetText(d, IDC_ST_HOSTSVC, L"running (demo)");
    else if (pn::QueryService(PN_HOST_SERVICE, si)) SetTextF(d, IDC_ST_HOSTSVC, L"%s (%s)", pn::ServiceStateName(si.state), si.startType == SERVICE_AUTO_START ? L"automatic start" : L"manual start");
    else SetText(d, IDC_ST_HOSTSVC, L"not installed (only SBC is available; run the installer or Driver > Install)");
    m_loading = guard;
}

void Panel::LoadCodecs()
{
    HWND d = m_page[PG_CODECS];
    HWND lv = GetDlgItem(d, IDC_CODEC_LIST);
    bool guard = m_loading; m_loading = true;
    int sel = ListViewSelected(lv);
    ULONG selectedCodec = sel >= 0 ? (ULONG)ListViewParam(lv, sel) : m_s.status.Codec;
    ListView_DeleteAllItems(lv);
    const PNA2DP_CONFIG &c = m_s.config;
    std::vector<ULONG> order;
    for (int i = 0; i < 8 && c.CodecPriority[i]; i++) {
        bool known = false; for (ULONG k : kCodecOrder) if (k == c.CodecPriority[i]) known = true;
        bool dup = false; for (ULONG k : order) if (k == c.CodecPriority[i]) dup = true;
        if (known && !dup) order.push_back(c.CodecPriority[i]);
    }
    for (ULONG k : kCodecOrder) { bool have = false; for (ULONG o : order) if (o == k) have = true; if (!have) order.push_back(k); }
    for (size_t i = 0; i < order.size(); i++) {
        ULONG codec = order[i];
        wchar_t num[8]; swprintf_s(num, L"%u", (unsigned)(i + 1));
        int row = ListViewAddRow(lv, num, (LPARAM)codec);
        ListViewSet(lv, row, 1, pn::CodecName(codec));
        ListView_SetCheckState(lv, row, (c.EnabledCodecMask & PNA2DP_CODEC_BIT(codec)) != 0 || codec == PNA2DP_CODEC_SBC);
        if (codec == selectedCodec) ListView_SetItemState(lv, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    if (ListViewSelected(lv) < 0 && !order.empty()) ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    Check(d, IDC_CODEC_HOSTONLY, (c.Flags & PNA2DP_CFG_HOST_CODECS_ONLY) != 0);
    Check(d, IDC_CODEC_APPLYLIVE, (c.Flags & PNA2DP_CFG_APPLY_LIVE) != 0);
    UpdateCodecLive();
    m_loading = guard;
}

void Panel::UpdateCodecLive()
{
    HWND d = m_page[PG_CODECS];
    if (!d) return;
    HWND lv = GetDlgItem(d, IDC_CODEC_LIST);
    PNA2DP_CONFIG displayed = m_s.config;
    if (m_dirty && !m_loading) CollectConfig(displayed);
    const auto &s = m_s.status;
    bool negotiated = m_s.open && s.Codec && s.State >= PNA2DP_STATE_CONFIGURED && s.State <= PNA2DP_STATE_SUSPENDED;
    std::wstring live = negotiated ? L"Live: " + std::wstring(pn::CodecName(s.Codec)) + L" | " + std::to_wstring(s.BitrateBps / 1000) +
        L" kbps | " + std::to_wstring(s.SampleRate) + L" Hz, " + std::to_wstring(s.Channels) + L" channels | " + pn::StateName(s.State) : L"Live: no codec negotiated";
    live += L"\r\nSaved policy: ";
    live += (m_s.applied.Flags & PNA2DP_CFG_AUTO_QUALITY) ? L"automatic adapter quality" : L"manual quality";
    if (m_dirty) live += L". Unsaved edits shown below; live output is unchanged until applied.";
    SetText(d, IDC_CODEC_LIVE, live);
    LVCOLUMNW column = {}; column.mask = LVCF_TEXT;
    column.pszText = const_cast<LPWSTR>(m_dirty ? L"Edited settings" : L"Saved settings");
    ListView_SetColumn(lv, 4, &column);
    for (int row = 0; row < ListView_GetItemCount(lv); ++row) {
        ULONG codec = (ULONG)ListViewParam(lv, row);
        bool supported = pn::SinkSupportsCodec(m_s.caps, codec);
        bool encoder = ((m_s.ver.KernelCodecMask | m_s.ver.HostCodecMask) & PNA2DP_CODEC_BIT(codec)) != 0;
        ListViewSet(lv, row, 2, !m_s.caps.SepCount ? L"Unknown" : supported ? L"Supported" : L"Unsupported");
        const wchar_t *state = negotiated && s.Codec == codec ? (s.State == PNA2DP_STATE_STREAMING ? L"Active" : L"Negotiated") :
            !m_s.caps.SepCount ? L"Not detected" : !supported ? L"Unavailable" : !encoder ? L"No encoder" :
            !(displayed.EnabledCodecMask & PNA2DP_CODEC_BIT(codec)) ? L"Disabled" : L"Ready";
        ListViewSet(lv, row, 3, state);
        ListViewSet(lv, row, 4, pn::CodecSettingsText(displayed, codec));
    }
    int selected = ListViewSelected(lv);
    ULONG codec = selected >= 0 ? (ULONG)ListViewParam(lv, selected) : 0;
    bool canActivate = m_s.open && pn::CanActivateCodec(m_s.caps, m_s.ver, codec);
    EnableItem(d, IDC_CODEC_ACTIVATE, canActivate);
    EnableItem(d, IDC_CODEC_UP, m_s.open && selected > 0);
    EnableItem(d, IDC_CODEC_DOWN, m_s.open && selected >= 0 && selected + 1 < ListView_GetItemCount(lv));
    std::wstring note;
    if (!m_s.open) note = L"Select a device using PN-A2DP to change codecs.";
    else if (!m_s.caps.SepCount) note = L"Receiver capabilities are not available yet. Connect the receiver before activating a codec.";
    else if (!codec) note = L"Select a codec to activate it. SBC remains available as a fallback.";
    else if (!pn::SinkSupportsCodec(m_s.caps, codec)) note = std::wstring(pn::CodecName(codec)) + L" is not advertised by this receiver and cannot be activated. Checked preferences do not add receiver support.";
    else if (!canActivate) note = L"The receiver supports this codec, but its encoder is unavailable. Start the codec host service.";
    else note = L"Activate selected saves the current edits and tries this codec first, with SBC fallback. The live row confirms what the receiver accepts.";
    SetText(d, IDC_CODEC_NOTE, note);
}

void Panel::UpdateCodecAvailability()
{
    /* Unknown capabilities are not evidence of an unsupported codec. */
    ULONG mask = m_s.caps.SepCount ? m_s.caps.CodecMask : ~0u;
    EnableWindow(m_page[PG_AAC], m_s.open && (mask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC)));
    EnableWindow(m_page[PG_APTX], m_s.open && (mask & (PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_HD) | PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX_LL))));
    EnableWindow(m_page[PG_LDAC], m_s.open && (mask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC)));
    UpdateCodecDetails();
}

void Panel::UpdateCodecDetails()
{
    const ULONG codecs[] = {PNA2DP_CODEC_SBC, PNA2DP_CODEC_AAC, PNA2DP_CODEC_APTX, PNA2DP_CODEC_LDAC};
    const int notes[] = {IDC_SBC_NOTE, IDC_AAC_NOTE, IDC_APTX_NOTE, IDC_LDAC_NOTE};
    for (int i = 0; i < 4; ++i) {
        std::wstring text = m_dirty ? L"Edited preferences (not applied). " : L"Saved preferences. ";
        text += L"Auto lets the driver choose compatible settings for the Windows audio format.\r\n\r\n";
        if (!m_s.open) text += L"No PN-A2DP device selected.";
        else if (i == 2) {
            for (ULONG c : {PNA2DP_CODEC_APTX, PNA2DP_CODEC_APTX_HD, PNA2DP_CODEC_APTX_LL})
                text += pn::CodecPageStatus(m_s.caps, m_s.status, c) + L"\r\n";
        } else text += pn::CodecPageStatus(m_s.caps, m_s.status, codecs[i]);
        SetText(m_page[PG_SBC + i], notes[i], text);
    }
    UpdateSbcBitrate();
}

void Panel::SaveCodecs(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_CODECS];
    HWND lv = GetDlgItem(d, IDC_CODEC_LIST);
    ZeroMemory(c.CodecPriority, sizeof(c.CodecPriority));
    ULONG mask = 0;
    int n = ListView_GetItemCount(lv);
    for (int i = 0; i < n && i < 8; i++) {
        ULONG codec = (ULONG)ListViewParam(lv, i);
        c.CodecPriority[i] = codec;
        if (ListView_GetCheckState(lv, i)) mask |= PNA2DP_CODEC_BIT(codec);
    }
    c.EnabledCodecMask = mask | PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
    if (Checked(d, IDC_CODEC_HOSTONLY)) c.Flags |= PNA2DP_CFG_HOST_CODECS_ONLY; else c.Flags &= ~PNA2DP_CFG_HOST_CODECS_ONLY;
    if (Checked(d, IDC_CODEC_APPLYLIVE)) c.Flags |= PNA2DP_CFG_APPLY_LIVE; else c.Flags &= ~PNA2DP_CFG_APPLY_LIVE;
}

void Panel::CodecMove(int delta)
{
    HWND lv = GetDlgItem(m_page[PG_CODECS], IDC_CODEC_LIST);
    int sel = ListViewSelected(lv);
    int n = ListView_GetItemCount(lv);
    if (sel < 0 || sel + delta < 0 || sel + delta >= n) return;
    /* swap into the config and reload the list */
    PNA2DP_CONFIG c; CollectConfig(c);
    ULONG t = c.CodecPriority[sel]; c.CodecPriority[sel] = c.CodecPriority[sel + delta]; c.CodecPriority[sel + delta] = t;
    m_s.config = c;
    LoadCodecs();
    ListView_SetItemState(lv, sel + delta, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SetDirty(true);
}

void Panel::LoadSbc()
{
    HWND d = m_page[PG_SBC];
    bool guard = m_loading; m_loading = true;
    const PNA2DP_SBC_PARAMS &s = m_s.config.Sbc;
    ULONG maxbp = s.MaxBitpool ? s.MaxBitpool : pn::SbcReceiverMaxBitpool(m_s.caps, m_s.status.Codec == PNA2DP_CODEC_SBC ? m_s.status.RemoteSeid : 0);
    SendMessageW(GetDlgItem(d, IDC_SBC_MAX_SLIDER), TBM_SETPOS, TRUE, maxbp);
    SetUInt(d, IDC_SBC_MAX_EDIT, maxbp);
    if (!maxbp) SetText(d, IDC_SBC_MAX_EDIT, L"");
    Check(d, IDC_SBC_MAX_SINK, s.MaxBitpool == 0);
    EnableItem(d, IDC_SBC_MAX_SLIDER, s.MaxBitpool != 0); EnableItem(d, IDC_SBC_MAX_EDIT, s.MaxBitpool != 0); EnableItem(d, IDC_SBC_MAX_SPIN, s.MaxBitpool != 0);
    SetUInt(d, IDC_SBC_MIN_EDIT, s.MinBitpool ? s.MinBitpool : 2);
    Check(d, IDC_SBC_OVERRIDE, s.AllowBitpoolOverride != 0);
    Check(d, IDC_SBC_DUAL, (m_s.config.Flags & PNA2DP_CFG_DUAL_CHANNEL_PREFER) != 0);
    Check(d, IDC_SBC_R16, (s.RateMask & PNA2DP_SBC_FREQ_16000) != 0); Check(d, IDC_SBC_R32, (s.RateMask & PNA2DP_SBC_FREQ_32000) != 0);
    Check(d, IDC_SBC_R44, (s.RateMask & PNA2DP_SBC_FREQ_44100) != 0); Check(d, IDC_SBC_R48, (s.RateMask & PNA2DP_SBC_FREQ_48000) != 0);
    Check(d, IDC_SBC_CMONO, (s.ChannelModeMask & PNA2DP_SBC_CH_MONO) != 0); Check(d, IDC_SBC_CDUAL, (s.ChannelModeMask & PNA2DP_SBC_CH_DUAL) != 0);
    Check(d, IDC_SBC_CSTEREO, (s.ChannelModeMask & PNA2DP_SBC_CH_STEREO) != 0); Check(d, IDC_SBC_CJOINT, (s.ChannelModeMask & PNA2DP_SBC_CH_JOINT) != 0);
    Check(d, IDC_SBC_B4, (s.BlockMask & PNA2DP_SBC_BLOCKS_4) != 0); Check(d, IDC_SBC_B8, (s.BlockMask & PNA2DP_SBC_BLOCKS_8) != 0);
    Check(d, IDC_SBC_B12, (s.BlockMask & PNA2DP_SBC_BLOCKS_12) != 0); Check(d, IDC_SBC_B16, (s.BlockMask & PNA2DP_SBC_BLOCKS_16) != 0);
    Check(d, IDC_SBC_S4, (s.SubbandMask & PNA2DP_SBC_SUBBANDS_4) != 0); Check(d, IDC_SBC_S8, (s.SubbandMask & PNA2DP_SBC_SUBBANDS_8) != 0);
    Check(d, IDC_SBC_ALOUD, (s.AllocMask & PNA2DP_SBC_ALLOC_LOUDNESS) != 0); Check(d, IDC_SBC_ASNR, (s.AllocMask & PNA2DP_SBC_ALLOC_SNR) != 0);
    SyncAutoGroups(d);
    UpdateSbcBitrate();
    m_loading = guard;
}

void Panel::SaveSbc(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_SBC];
    PNA2DP_SBC_PARAMS &s = c.Sbc;
    s.MaxBitpool = Checked(d, IDC_SBC_MAX_SINK) ? 0 : (UCHAR)min(250u, max(2u, GetUInt(d, IDC_SBC_MAX_EDIT)));
    s.MinBitpool = (UCHAR)min(250u, max(2u, GetUInt(d, IDC_SBC_MIN_EDIT)));
    s.AllowBitpoolOverride = Checked(d, IDC_SBC_OVERRIDE) ? 1 : 0;
    if (Checked(d, IDC_SBC_DUAL)) c.Flags |= PNA2DP_CFG_DUAL_CHANNEL_PREFER; else c.Flags &= ~PNA2DP_CFG_DUAL_CHANNEL_PREFER;
    s.RateMask = (Checked(d, IDC_SBC_R16) ? PNA2DP_SBC_FREQ_16000 : 0) | (Checked(d, IDC_SBC_R32) ? PNA2DP_SBC_FREQ_32000 : 0) | (Checked(d, IDC_SBC_R44) ? PNA2DP_SBC_FREQ_44100 : 0) | (Checked(d, IDC_SBC_R48) ? PNA2DP_SBC_FREQ_48000 : 0);
    s.ChannelModeMask = (Checked(d, IDC_SBC_CMONO) ? PNA2DP_SBC_CH_MONO : 0) | (Checked(d, IDC_SBC_CDUAL) ? PNA2DP_SBC_CH_DUAL : 0) | (Checked(d, IDC_SBC_CSTEREO) ? PNA2DP_SBC_CH_STEREO : 0) | (Checked(d, IDC_SBC_CJOINT) ? PNA2DP_SBC_CH_JOINT : 0);
    s.BlockMask = (Checked(d, IDC_SBC_B4) ? PNA2DP_SBC_BLOCKS_4 : 0) | (Checked(d, IDC_SBC_B8) ? PNA2DP_SBC_BLOCKS_8 : 0) | (Checked(d, IDC_SBC_B12) ? PNA2DP_SBC_BLOCKS_12 : 0) | (Checked(d, IDC_SBC_B16) ? PNA2DP_SBC_BLOCKS_16 : 0);
    s.SubbandMask = (Checked(d, IDC_SBC_S4) ? PNA2DP_SBC_SUBBANDS_4 : 0) | (Checked(d, IDC_SBC_S8) ? PNA2DP_SBC_SUBBANDS_8 : 0);
    s.AllocMask = (Checked(d, IDC_SBC_ALOUD) ? PNA2DP_SBC_ALLOC_LOUDNESS : 0) | (Checked(d, IDC_SBC_ASNR) ? PNA2DP_SBC_ALLOC_SNR : 0);
}

void Panel::UpdateSbcBitrate()
{
    HWND d = m_page[PG_SBC];
    ULONG bp = Checked(d, IDC_SBC_MAX_SINK) ? pn::SbcReceiverMaxBitpool(m_s.caps, m_s.status.Codec == PNA2DP_CODEC_SBC ? m_s.status.RemoteSeid : 0) : min(250u, max(2u, GetUInt(d, IDC_SBC_MAX_EDIT)));
    if (Checked(d, IDC_SBC_MAX_SINK)) {
        std::wstring value = bp ? std::to_wstring(bp) : L"";
        if (GetText(d, IDC_SBC_MAX_EDIT) != value) {
            bool guard = m_loading; m_loading = true;
            SetText(d, IDC_SBC_MAX_EDIT, value);
            SendMessageW(GetDlgItem(d, IDC_SBC_MAX_SLIDER), TBM_SETPOS, TRUE, bp);
            m_loading = guard;
        }
    }
    if (!bp) { SetText(d, IDC_SBC_BITRATE, L"Receiver maximum: unknown until SBC capabilities are available. No bitrate is assumed."); return; }
    ULONG blocks = Checked(d, IDC_SBC_B16) || !(Checked(d, IDC_SBC_B4) || Checked(d, IDC_SBC_B8) || Checked(d, IDC_SBC_B12)) ? 16 : Checked(d, IDC_SBC_B12) ? 12 : Checked(d, IDC_SBC_B8) ? 8 : 4;
    ULONG subs = Checked(d, IDC_SBC_S4) && !Checked(d, IDC_SBC_S8) ? 4 : 8;
    bool dual = Checked(d, IDC_SBC_DUAL) || (Checked(d, IDC_SBC_CDUAL) && !Checked(d, IDC_SBC_CJOINT) && !Checked(d, IDC_SBC_CSTEREO));
    ULONG mode = dual ? PNA2DP_SBC_CH_DUAL : Checked(d, IDC_SBC_CJOINT) || Checked(d, IDC_SBC_CAUTO) ? PNA2DP_SBC_CH_JOINT : Checked(d, IDC_SBC_CSTEREO) ? PNA2DP_SBC_CH_STEREO : PNA2DP_SBC_CH_MONO;
    const wchar_t *modeName = mode == PNA2DP_SBC_CH_DUAL ? L"dual channel" : mode == PNA2DP_SBC_CH_JOINT ? L"joint stereo" : mode == PNA2DP_SBC_CH_STEREO ? L"stereo" : L"mono";
    SetTextF(d, IDC_SBC_BITRATE, L"Estimated bitrate at bitpool %u, %u blocks, %u subbands, %s: %u kbps at 44.1 kHz, %u kbps at 48 kHz%s.", bp, blocks, subs,
             modeName, (pn::SbcBitrateBps(44100, subs, blocks, mode, bp) + 500) / 1000, (pn::SbcBitrateBps(48000, subs, blocks, mode, bp) + 500) / 1000,
             Checked(d, IDC_SBC_MAX_SINK) ? L" (receiver-advertised bitpool)" : L"");
}

void Panel::LoadAac()
{
    HWND d = m_page[PG_AAC];
    bool guard = m_loading; m_loading = true;
    const PNA2DP_AAC_PARAMS &a = m_s.config.Aac;
    SetUInt(d, IDC_AAC_BITRATE, a.BitrateBps / 1000);
    if (!a.BitrateBps) SetText(d, IDC_AAC_BITRATE, L"");
    Check(d, IDC_AAC_SINKMAX, a.BitrateBps == 0);
    EnableItem(d, IDC_AAC_BITRATE, a.BitrateBps != 0);
    SetUInt(d, IDC_AAC_PEAK, a.PeakBitrateBps / 1000);
    Check(d, IDC_AAC_VBR, a.Vbr != 0);
    ComboBox_SetCurSel(GetDlgItem(d, IDC_AAC_OBJECT), a.ObjectType == 2 ? 1 : 0);
    Check(d, IDC_AAC_AFTERBURNER, a.Afterburner != 0);
    Check(d, IDC_AAC_R16, (a.RateMask & PNA2DP_RATE_16000) != 0); Check(d, IDC_AAC_R32, (a.RateMask & PNA2DP_RATE_32000) != 0);
    Check(d, IDC_AAC_R44, (a.RateMask & PNA2DP_RATE_44100) != 0); Check(d, IDC_AAC_R48, (a.RateMask & PNA2DP_RATE_48000) != 0);
    Check(d, IDC_AAC_R88, (a.RateMask & PNA2DP_RATE_88200) != 0); Check(d, IDC_AAC_R96, (a.RateMask & PNA2DP_RATE_96000) != 0);
    Check(d, IDC_AAC_CMONO, (a.ChannelModeMask & PNA2DP_CHMODE_MONO) != 0); Check(d, IDC_AAC_CSTEREO, (a.ChannelModeMask & PNA2DP_CHMODE_STEREO) != 0);
    SyncAutoGroups(d);
    m_loading = guard;
}

void Panel::SaveAac(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_AAC];
    PNA2DP_AAC_PARAMS &a = c.Aac;
    a.BitrateBps = Checked(d, IDC_AAC_SINKMAX) ? 0 : GetUInt(d, IDC_AAC_BITRATE) * 1000;
    a.PeakBitrateBps = GetUInt(d, IDC_AAC_PEAK) * 1000;
    a.Vbr = Checked(d, IDC_AAC_VBR) ? 1 : 0;
    int o = ComboBox_GetCurSel(GetDlgItem(d, IDC_AAC_OBJECT));
    a.ObjectType = o == 1 ? 2 : 0;
    a.Afterburner = Checked(d, IDC_AAC_AFTERBURNER) ? 1 : 0;
    a.RateMask = (Checked(d, IDC_AAC_R16) ? PNA2DP_RATE_16000 : 0) | (Checked(d, IDC_AAC_R32) ? PNA2DP_RATE_32000 : 0) | (Checked(d, IDC_AAC_R44) ? PNA2DP_RATE_44100 : 0) |
                 (Checked(d, IDC_AAC_R48) ? PNA2DP_RATE_48000 : 0) | (Checked(d, IDC_AAC_R88) ? PNA2DP_RATE_88200 : 0) | (Checked(d, IDC_AAC_R96) ? PNA2DP_RATE_96000 : 0);
    a.ChannelModeMask = (Checked(d, IDC_AAC_CMONO) ? PNA2DP_CHMODE_MONO : 0) | (Checked(d, IDC_AAC_CSTEREO) ? PNA2DP_CHMODE_STEREO : 0);
}

void Panel::LoadAptx()
{
    HWND d = m_page[PG_APTX];
    bool guard = m_loading; m_loading = true;
    const PNA2DP_APTX_PARAMS *p[3] = { &m_s.config.Aptx, &m_s.config.AptxHd, &m_s.config.AptxLl };
    for (int v = 0; v < 3; v++) {
        Check(d, IDC_APTX_ID(v, IDC_APTX_R16), (p[v]->RateMask & PNA2DP_RATE_16000) != 0);
        Check(d, IDC_APTX_ID(v, IDC_APTX_R32), (p[v]->RateMask & PNA2DP_RATE_32000) != 0);
        Check(d, IDC_APTX_ID(v, IDC_APTX_R44), (p[v]->RateMask & PNA2DP_RATE_44100) != 0);
        Check(d, IDC_APTX_ID(v, IDC_APTX_R48), (p[v]->RateMask & PNA2DP_RATE_48000) != 0);
        Check(d, IDC_APTX_ID(v, IDC_APTX_CMONO), false);
        Check(d, IDC_APTX_ID(v, IDC_APTX_CSTEREO), true); // encoder always requires stereo
    }
    Check(d, IDC_APTX_ID(1, IDC_APTX_F16), false);
    Check(d, IDC_APTX_ID(1, IDC_APTX_F24), true); // fixed 24-bit encoder input
    SetUInt(d, IDC_APTX_ID(2, IDC_APTX_LLTARGET), m_s.config.AptxLl.LlTargetLevel);
    SyncAutoGroups(d);
    m_loading = guard;
}

void Panel::SaveAptx(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_APTX];
    PNA2DP_APTX_PARAMS *p[3] = { &c.Aptx, &c.AptxHd, &c.AptxLl };
    for (int v = 0; v < 3; v++) {
        p[v]->RateMask = (Checked(d, IDC_APTX_ID(v, IDC_APTX_R16)) ? PNA2DP_RATE_16000 : 0) | (Checked(d, IDC_APTX_ID(v, IDC_APTX_R32)) ? PNA2DP_RATE_32000 : 0) |
                         (Checked(d, IDC_APTX_ID(v, IDC_APTX_R44)) ? PNA2DP_RATE_44100 : 0) | (Checked(d, IDC_APTX_ID(v, IDC_APTX_R48)) ? PNA2DP_RATE_48000 : 0);
        // Disabled fields show encoder requirements; retain their reserved stored values.
    }
    c.AptxLl.LlTargetLevel = (UCHAR)min(255u, GetUInt(d, IDC_APTX_ID(2, IDC_APTX_LLTARGET)));
}

void Panel::LoadLdac()
{
    HWND d = m_page[PG_LDAC];
    bool guard = m_loading; m_loading = true;
    const PNA2DP_LDAC_PARAMS &l = m_s.config.Ldac;
    CheckRadioButton(d, IDC_LDAC_QHQ, IDC_LDAC_QABR, l.Eqmid == PNA2DP_LDAC_EQMID_HQ ? IDC_LDAC_QHQ : l.Eqmid == PNA2DP_LDAC_EQMID_MQ ? IDC_LDAC_QMQ : l.Eqmid == PNA2DP_LDAC_EQMID_ABR ? IDC_LDAC_QABR : IDC_LDAC_QSQ);
    Check(d, IDC_LDAC_ABR, l.AbrEnable != 0);
    int f = (l.FormatMask & PNA2DP_FMT_S32) ? IDC_LDAC_F32 : (l.FormatMask & PNA2DP_FMT_S24) ? IDC_LDAC_F24 : (l.FormatMask & PNA2DP_FMT_F32) ? IDC_LDAC_FF32 : (l.FormatMask & PNA2DP_FMT_S16) ? IDC_LDAC_F16 : IDC_LDAC_FAUTO;
    CheckRadioButton(d, IDC_LDAC_F16, IDC_LDAC_FAUTO, f);
    Check(d, IDC_LDAC_R44, (l.RateMask & PNA2DP_RATE_44100) != 0); Check(d, IDC_LDAC_R48, (l.RateMask & PNA2DP_RATE_48000) != 0);
    Check(d, IDC_LDAC_R88, (l.RateMask & PNA2DP_RATE_88200) != 0); Check(d, IDC_LDAC_R96, (l.RateMask & PNA2DP_RATE_96000) != 0);
    Check(d, IDC_LDAC_CMONO, (l.ChannelModeMask & PNA2DP_CHMODE_MONO) != 0); Check(d, IDC_LDAC_CDUAL, (l.ChannelModeMask & PNA2DP_CHMODE_DUAL) != 0);
    Check(d, IDC_LDAC_CSTEREO, (l.ChannelModeMask & PNA2DP_CHMODE_STEREO) != 0);
    SyncAutoGroups(d);
    UpdateLdacBitrate();
    m_loading = guard;
}

void Panel::SaveLdac(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_LDAC];
    PNA2DP_LDAC_PARAMS &l = c.Ldac;
    l.Eqmid = Checked(d, IDC_LDAC_QHQ) ? PNA2DP_LDAC_EQMID_HQ : Checked(d, IDC_LDAC_QMQ) ? PNA2DP_LDAC_EQMID_MQ : Checked(d, IDC_LDAC_QABR) ? PNA2DP_LDAC_EQMID_ABR : PNA2DP_LDAC_EQMID_SQ;
    l.AbrEnable = Checked(d, IDC_LDAC_ABR) ? 1 : 0;
    l.FormatMask = Checked(d, IDC_LDAC_F16) ? PNA2DP_FMT_S16 : Checked(d, IDC_LDAC_F24) ? PNA2DP_FMT_S24 : Checked(d, IDC_LDAC_F32) ? PNA2DP_FMT_S32 : Checked(d, IDC_LDAC_FF32) ? PNA2DP_FMT_F32 : 0;
    l.RateMask = (Checked(d, IDC_LDAC_R44) ? PNA2DP_RATE_44100 : 0) | (Checked(d, IDC_LDAC_R48) ? PNA2DP_RATE_48000 : 0) | (Checked(d, IDC_LDAC_R88) ? PNA2DP_RATE_88200 : 0) | (Checked(d, IDC_LDAC_R96) ? PNA2DP_RATE_96000 : 0);
    l.ChannelModeMask = (Checked(d, IDC_LDAC_CMONO) ? PNA2DP_CHMODE_MONO : 0) | (Checked(d, IDC_LDAC_CDUAL) ? PNA2DP_CHMODE_DUAL : 0) | (Checked(d, IDC_LDAC_CSTEREO) ? PNA2DP_CHMODE_STEREO : 0);
}

void Panel::UpdateLdacBitrate()
{
    HWND d = m_page[PG_LDAC];
    ULONG q = Checked(d, IDC_LDAC_QHQ) ? 0 : Checked(d, IDC_LDAC_QMQ) ? 2 : Checked(d, IDC_LDAC_QABR) ? PNA2DP_LDAC_EQMID_ABR : 1;
    SetTextF(d, IDC_LDAC_BITRATE, L"Starting bitrate: %u kbps at 48 / 96 kHz, %u kbps at 44.1 / 88.2 kHz%s.", pn::LdacBitrateBps(q, 48000) / 1000, pn::LdacBitrateBps(q, 44100) / 1000,
             Checked(d, IDC_LDAC_ABR) ? L"; ABR may step down to 330 kbps" : L"");
}

void Panel::LoadTransport()
{
    HWND d = m_page[PG_TRANSPORT];
    bool guard = m_loading; m_loading = true;
    const PNA2DP_CONFIG &c = m_s.config;
    SendMessageW(GetDlgItem(d, IDC_TR_TARGET_SLIDER), TBM_SETPOS, TRUE, c.TargetLatencyMs);
    SetUInt(d, IDC_TR_TARGET_EDIT, c.TargetLatencyMs);
    SendMessageW(GetDlgItem(d, IDC_TR_MAX_SLIDER), TBM_SETPOS, TRUE, c.MaxLatencyMs);
    SetUInt(d, IDC_TR_MAX_EDIT, c.MaxLatencyMs);
    Check(d, IDC_TR_ABR, (c.Flags & PNA2DP_CFG_ABR_ENABLE) != 0);
    SetUInt(d, IDC_TR_ABRFLOOR, c.AbrFloorBps / 1000);
    SetUInt(d, IDC_TR_RECONNECT, c.ReconnectDelayMs);
    Check(d, IDC_TR_SUSPENDSCO, (c.Flags & PNA2DP_CFG_SUSPEND_ON_SCO) != 0);
    Check(d, IDC_TR_AUTOSTART, (c.Flags & PNA2DP_CFG_AUTO_START_ON_CONNECT) != 0);
    Check(d, IDC_TR_DELAYREPORT, (c.Flags & PNA2DP_CFG_DELAY_REPORTING) != 0);
    Check(d, IDC_TR_ALLRATES, (c.Flags & PNA2DP_CFG_EXPOSE_ALL_RATES) != 0);
    Check(d, IDC_TR_SWVOLUME, true);
    SetDlgItemInt(d, IDC_TR_VOLUME, c.VolumeDb16 / 65536, TRUE);
    m_volumeLoaded = c.VolumeDb16;
    m_volumeEdited = false;
    m_loading = guard;
}

void Panel::SaveTransport(PNA2DP_CONFIG &c)
{
    HWND d = m_page[PG_TRANSPORT];
    c.TargetLatencyMs = GetUInt(d, IDC_TR_TARGET_EDIT);
    c.MaxLatencyMs = GetUInt(d, IDC_TR_MAX_EDIT);
    auto flag = [&](int id, ULONG bit) { if (Checked(d, id)) c.Flags |= bit; else c.Flags &= ~bit; };
    flag(IDC_TR_ABR, PNA2DP_CFG_ABR_ENABLE);
    flag(IDC_TR_SUSPENDSCO, PNA2DP_CFG_SUSPEND_ON_SCO);
    flag(IDC_TR_AUTOSTART, PNA2DP_CFG_AUTO_START_ON_CONNECT);
    flag(IDC_TR_DELAYREPORT, PNA2DP_CFG_DELAY_REPORTING);
    flag(IDC_TR_ALLRATES, PNA2DP_CFG_EXPOSE_ALL_RATES);
    flag(IDC_TR_SWVOLUME, PNA2DP_CFG_SOFTWARE_VOLUME);
    c.AbrFloorBps = GetUInt(d, IDC_TR_ABRFLOOR) * 1000;
    c.ReconnectDelayMs = GetUInt(d, IDC_TR_RECONNECT);
    BOOL ok = FALSE;
    int db = (int)GetDlgItemInt(d, IDC_TR_VOLUME, &ok, TRUE);
    /* only an edited value is written back; otherwise the (fractional) value stays as loaded */
    if (ok && db != m_volumeLoaded / 65536) { if (db > 0) db = 0; if (db < -96) db = -96; c.VolumeDb16 = db * 65536; m_volumeEdited = true; }
    else c.VolumeDb16 = m_volumeLoaded;
}

void Panel::LoadSink()
{
    HWND d = m_page[PG_SINK];
    HWND lv = GetDlgItem(d, IDC_SINK_LIST);
    bool guard = m_loading; m_loading = true;
    ListView_DeleteAllItems(lv);
    SetText(d, IDC_SINK_DETAIL, L"");
    const PNA2DP_CAPABILITIES &c = m_s.caps;
    if (!m_s.open || !c.SepCount) {
        SetText(d, IDC_SINK_INFO, m_s.open ? L"The headset's capabilities are not known yet (it is discovered when it comes into range)." : L"No PN-A2DP driver instance selected.");
    } else {
        SetTextF(d, IDC_SINK_INFO, L"AVDTP %x.%x, SDP features 0x%04X, %u stream endpoints, codecs: %s", c.SdpProfileVersion >> 8, c.SdpProfileVersion & 0xFF, c.SdpSupportedFeatures, c.SepCount,
                 [&]() { std::wstring s; for (ULONG k = 1; k < PNA2DP_CODEC_MAX; k++) if (c.CodecMask & PNA2DP_CODEC_BIT(k)) { if (!s.empty()) s += L", "; s += pn::CodecName(k); } return s; }().c_str());
        for (ULONG i = 0; i < c.SepCount && i < PNA2DP_MAX_REMOTE_SEPS; i++) {
            const PNA2DP_REMOTE_SEP &s = c.Seps[i];
            wchar_t b[16]; swprintf_s(b, L"%u", s.Seid);
            int row = ListViewAddRow(lv, b, (LPARAM)i);
            std::wstring name = pn::CodecName(s.Codec);
            if (s.Codec == PNA2DP_CODEC_NONE) { wchar_t v[64]; swprintf_s(v, L"vendor %04X/%04X", s.VendorId, s.VendorCodecId); name = v; }
            ListViewSet(lv, row, 1, name);
            std::wstring desc = pn::DescribeSep(s);
            auto field = [&](const wchar_t *key) -> std::wstring {
                size_t p = desc.find(key); if (p == std::wstring::npos) return L"-";
                p += wcslen(key); size_t e = desc.find(L"\r\n", p); return desc.substr(p, e == std::wstring::npos ? std::wstring::npos : e - p);
            };
            ListViewSet(lv, row, 2, field(L"Sample rates: "));
            std::wstring ch = field(L"Channel modes: "); if (ch == L"-") ch = field(L"Channels: ");
            ListViewSet(lv, row, 3, ch);
            std::wstring fl;
            if (s.InUse) fl += L"in use ";
            if (s.ServiceCaps & PNA2DP_SVC_DELAY_REPORTING) fl += L"delay-reporting ";
            if (s.ServiceCaps & PNA2DP_SVC_CONTENT_PROTECTION) fl += L"SCMS-T ";
            if (!s.TsepIsSink) fl += L"source ";
            ListViewSet(lv, row, 4, fl);
        }
        ListView_SetItemState(lv, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        SetText(d, IDC_SINK_DETAIL, pn::DescribeSep(c.Seps[0]));
    }
    m_loading = guard;
}

void Panel::LoadDriver()
{
    HWND d = m_page[PG_DRIVER];
    bool guard = m_loading; m_loading = true;
    auto staged = pn::FindStagedPackages();
    std::wstring candidate = staged.empty() ? FindPackageInf() : staged.back().fullPath;
    m_driverTrust = candidate.empty() ? pn::PackageTrust{} : pn::VerifyPnInf(candidate);
    if (candidate.empty()) m_driverTrust.detail = L"No PN driver package is available. Use Stage package or install the full development package.";
    bool haveNode = m_sel >= 0 && m_sel < (int)m_devices.size();
    const DeviceEntry *e = haveNode ? &m_devices[m_sel] : nullptr;
    if (e) {
        pn::A2dpNode n = e->node;
        if (!e->demo) pn::GetA2dpNode(e->node.instanceId, n);
        SetText(d, IDC_DRV_CURRENT, n.driverDesc.empty() ? L"(no driver)" : n.driverDesc);
        SetText(d, IDC_DRV_PROVIDER, n.driverProvider.empty() ? L"-" : n.driverProvider);
        SetText(d, IDC_DRV_VERSION, n.driverVersion.empty() ? L"-" : n.driverVersion);
        SetText(d, IDC_DRV_INF, n.driverInf.empty() ? L"-" : n.driverInf);
        std::wstring st = n.present ? L"present" : L"not connected (driver switch applies when it reconnects)";
        if (n.restartRequired) st += L" - restart Windows required";
        if (n.problemCode) { wchar_t b[64]; swprintf_s(b, L" - problem code %u", n.problemCode); st += b; }
        SetText(d, IDC_DRV_STATUS, st);
        bool inbox = _wcsicmp(n.driverInf.c_str(), PN_INBOX_A2DP_INF) == 0;
        bool newer = !staged.empty() && pn::CompareDriverVersions(staged.back().version, n.driverVersion) > 0;
        SetText(d, IDC_DRV_USE_PNA2DP, n.usesPnA2dp() && newer ? L"Update PN-A2DP driver" : L"Use PN-A2DP driver");
        m_canSwitchPn = !n.usesPnA2dp() || newer;
        m_canSwitchWindows = !inbox && !n.driverInf.empty();
    } else {
        for (int id : { IDC_DRV_CURRENT, IDC_DRV_PROVIDER, IDC_DRV_VERSION, IDC_DRV_INF, IDC_DRV_STATUS }) SetText(d, id, L"-");
        m_canSwitchPn = m_canSwitchWindows = false;
    }
    if (staged.empty()) {
        std::wstring src = FindPackageInf();
        SetText(d, IDC_DRV_PKG, src.empty() ? L"Package: not staged in the driver store and no pna2dp.inf found next to this program." : L"Package: not staged; will be staged from " + src + L" when needed.");
    } else {
        std::wstring s = L"Package: staged as " + staged.back().publishedName + L" (version " + staged.back().version + L")";
        if (staged.size() > 1) { wchar_t b[48]; swprintf_s(b, L" + %u other packages", (unsigned)(staged.size() - 1)); s += b; }
        SetText(d, IDC_DRV_PKG, s);
    }
    EnableItem(d, IDC_DRV_UNSTAGE, !staged.empty());
    LoadReadiness();
    pn::ServiceInfo si;
    bool have = pn::QueryService(PN_HOST_SERVICE, si);
    if (m_demo && !have) { have = true; si.installed = true; si.state = SERVICE_RUNNING; si.startType = SERVICE_AUTO_START; }
    SetTextF(d, IDC_DRV_SVC, L"Service %s: %s%s", PN_HOST_SERVICE, have ? pn::ServiceStateName(si.state) : L"not installed", have ? (si.startType == SERVICE_AUTO_START ? L", automatic start" : L", manual start") : L"");
    EnableItem(d, IDC_DRV_SVC_START, have && si.state != SERVICE_RUNNING);
    EnableItem(d, IDC_DRV_SVC_STOP, have && si.state == SERVICE_RUNNING);
    EnableItem(d, IDC_DRV_SVC_REMOVE, have);
    std::wstring dir = pn::InstallDir();
    std::wstring hostExe = pn::JoinPath(dir, L"pna2dp-codechost.exe");
    if (!pn::FileExists(hostExe)) { std::wstring local = pn::JoinPath(pn::ModuleDir(), L"pna2dp-codechost.exe"); if (pn::FileExists(local)) { dir = pn::ModuleDir(); hostExe = local; } }
    EnableItem(d, IDC_DRV_SVC_INSTALL, pn::FileExists(hostExe));
    SetText(d, IDC_DRV_INSTALLDIR, dir);
    HWND lv = GetDlgItem(d, IDC_DRV_PLUGINS);
    ListView_DeleteAllItems(lv);
    auto plugins = pn::EnumeratePlugins(dir);
    for (auto &p : plugins) {
        int row = ListViewAddRow(lv, pn::BaseName(p.file));
        ListViewSet(lv, row, 1, p.valid ? p.version : L"-");
        ListViewSet(lv, row, 2, p.valid ? p.license : p.name);
        std::wstring codecs;
        for (ULONG k = 1; k < PNA2DP_CODEC_MAX; k++) if (p.codecMask & PNA2DP_CODEC_BIT(k)) { if (!codecs.empty()) codecs += L", "; codecs += pn::CodecName(k); }
        ListViewSet(lv, row, 3, p.valid ? codecs : L"invalid plugin");
    }
    if (plugins.empty()) { int row = ListViewAddRow(lv, L"(built-in SBC only)"); ListViewSet(lv, row, 3, L"no pna2dp-codec-*.dll in the folder"); }
    m_loading = guard;
}

void Panel::LoadReadiness()
{
    HWND d = m_page[PG_DRIVER];
    m_signing = pn::QuerySigningState();
    m_readiness = pn::EvaluateDriverReadiness(m_signing, m_driverTrust);
    bool bootOpen = BootSettingsWindow() != nullptr;
    if (bootOpen) {
        m_readiness.error = ERROR_BUSY;
        m_readiness.detail = L"Close the Windows / Test Mode window before switching drivers. If you changed the boot mode, restart Windows manually when required, then retry.\r\n\r\n" + m_readiness.detail;
    }
    const auto &s = m_signing;
    SetText(d, IDC_DRV_TEST_LIGHT, std::wstring(L"Test Mode: ") + (!s.queried ? L"Unknown" : s.testSigning ? L"ON" : L"OFF"));
    SetText(d, IDC_DRV_SECURE_LIGHT, std::wstring(L"Secure Boot: ") + (!s.secureBootKnown ? L"Unknown" : s.secureBoot ? L"ON" : L"OFF"));
    SetText(d, IDC_DRV_HVCI_LIGHT, std::wstring(L"Memory Integrity: ") + (!s.queried ? L"Unknown" : s.hvci ? L"ON" : L"OFF"));
    for (int id : {IDC_DRV_TEST_LIGHT, IDC_DRV_SECURE_LIGHT, IDC_DRV_HVCI_LIGHT}) InvalidateRect(GetDlgItem(d, id), nullptr, FALSE);
    std::wstring summary = bootOpen ? L"Close Windows / Test Mode before switching drivers." :
        !m_readiness.ready() ? L"Activation blocked. Open Requirements for the steps to finish setup." :
        m_driverTrust.microsoftDriver ? L"Microsoft signature verified. These security features can stay enabled." :
        L"Ready: development driver requires Test Mode ON, Secure Boot OFF and Memory Integrity OFF.";
    if (s.queried && s.hvciConfiguredKnown && s.hvci != s.hvciConfigured)
        summary += L"\r\nMemory Integrity has a pending change; restart manually and recheck.";
    SetText(d, IDC_DRV_READINESS, summary);
    EnableItem(d, IDC_DRV_USE_PNA2DP, m_canSwitchPn && m_readiness.ready());
    EnableItem(d, IDC_DRV_USE_INBOX, m_canSwitchWindows && !bootOpen);
}

/* ---- page window procedure ------------------------------------------------ */

INT_PTR CALLBACK Panel::PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    Panel *p = g_panel;
    switch (msg) {
    case WM_INITDIALOG: SetWindowLongPtrW(hwnd, DWLP_USER, lp); return TRUE;
    case WM_DRAWITEM: if (p->m_styleDpi) { p->DrawControl(*(DRAWITEMSTRUCT *)lp); return TRUE; } break;
    case WM_CTLCOLORDLG: return (INT_PTR)p->m_surface;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLORBTN: return p->ControlColor(msg, (HDC)wp, (HWND)lp);
    case WM_COMMAND: return p->OnPageCommand(hwnd, LOWORD(wp), HIWORD(wp), (HWND)lp);
    case WM_NOTIFY: return p->OnPageNotify(hwnd, (NMHDR *)lp);
    case WM_HSCROLL: return p->OnPageScroll(hwnd, (HWND)lp);
    }
    return FALSE;
}

INT_PTR Panel::OnPageCommand(HWND page, int id, int code, HWND)
{
    if (m_inPrompt) return TRUE;
    if (page == m_page[PG_QUALITY]) {
        if (id == IDC_HEALTH_CHECK) { CheckAudio(); return TRUE; }
        if (id == IDC_HEALTH_EXPORT) { ExportDiagnostics(); return TRUE; }
        if (id == IDC_HEALTH_FIX) { OpenHealthAction(); return TRUE; }
        if (id == IDC_QUALITY_EVENTS) { ShowQualityEvents(); return TRUE; }
        return TRUE; // diagnostics never create editable codec preferences
    }
    /* Windows controls apply immediately and never mark codec settings dirty. */
    if (page == m_page[PG_WINDOWS]) {
        if (id == IDC_WIN_REFRESH) { LoadWindowsAudio(); return TRUE; }
        if (id == IDC_WIN_DETAILS) { Prompt(pn::WindowsAudioReport(m_audio)); return TRUE; }
        if ((id == IDC_WIN_SOUND || id == IDC_WIN_BLUETOOTH) && !m_demo) {
            HINSTANCE result = ShellExecuteW(m_hwnd, L"open", id == IDC_WIN_SOUND ? L"ms-settings:sound" : L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
            if ((INT_PTR)result <= 32) Prompt(L"Cannot open Windows settings.", MB_ICONERROR);
            return TRUE;
        }
        if (id == IDC_WIN_MUTE && code == BN_CLICKED && WindowsMode() && !m_demo) {
            const unsigned generation = m_sessionGen, revision = ++m_audioRevision;
            HRESULT hr = pn::SetWindowsMute(m_devices[m_sel].node.instanceId, m_audio.endpointId, Checked(page, id));
            if (generation != m_sessionGen || revision != m_audioRevision) return TRUE;
            LoadWindowsAudio();
            if (SUCCEEDED(hr)) SetStatusText(L"Output mute updated.");
            if (FAILED(hr)) SetStatusText(L"Mute was not changed. The output may have disconnected or changed; refresh and retry.");
            return TRUE;
        }
        return TRUE;
    }
    /* page specific actions first */
    if (page == m_page[PG_ADAPTER] && id == IDC_AD_BEST) {
        PNA2DP_CONFIG c; CollectConfig(c);
        pn::UseAutomaticCodecs(c);
        m_s.config = c; LoadAll(); SetDirty(true);
        if (Apply()) SetStatusText(L"Automatic quality enabled: receiver and adapter capabilities, with continuous transport monitoring.");
        return TRUE;
    }
    if (page == m_page[PG_STATUS]) {
        if (id == IDC_ST_RESETSTATS) { if (!m_s.demo) m_s.dev.ResetStats(); else m_s.stats = PNA2DP_STATS{}; m_s.Refresh(false); LoadStatus(); return TRUE; }
        if (id == IDC_ST_RECONNECT) { if (!m_s.demo && !m_s.dev.Reconnect()) Error(L"Reconnect failed.", m_s.dev.LastError()); else SetStatusText(L"Reconnecting..."); return TRUE; }
        return FALSE;
    }
    if (page == m_page[PG_CODECS]) {
        if (id == IDC_CODEC_UP) { CodecMove(-1); return TRUE; }
        if (id == IDC_CODEC_DOWN) { CodecMove(+1); return TRUE; }
        if (id == IDC_CODEC_ENABLEALL) {
            PNA2DP_CONFIG c; CollectConfig(c);
            pn::UseAutomaticCodecs(c);
            m_s.config = c; LoadAll(); SetDirty(true);
            if (Apply()) SetStatusText(L"Automatic codec selection enabled. The driver detects this device's capabilities and skips unsupported codecs.");
            return TRUE;
        }
        if (id == IDC_PRESET && code == CBN_SELCHANGE) {
            int selected = ComboBox_GetCurSel(GetDlgItem(page, IDC_PRESET));
            EnableItem(page, IDC_PRESET_APPLY, m_s.open && selected > 0);
            SetText(page, IDC_PRESET_DESC, selected > 0 ? pn::PresetDescription((pn::Preset)(selected - 1)) : L"Choose a preset to preview; this does not change saved settings.");
            return TRUE;
        }
        if (id == IDC_CODEC_ACTIVATE) {
            int selected = ListViewSelected(GetDlgItem(page, IDC_CODEC_LIST));
            ULONG codec = selected >= 0 ? (ULONG)ListViewParam(GetDlgItem(page, IDC_CODEC_LIST), selected) : 0;
            PNA2DP_CONFIG c; CollectConfig(c);
            if (!m_s.open || !pn::PreferCodec(c, m_s.caps, m_s.ver, codec)) { UpdateCodecLive(); return TRUE; }
            m_s.config = c; LoadAll(); SetDirty(true);
            if (Apply()) SetStatusText(L"Requested " + std::wstring(pn::CodecName(codec)) + L" first with SBC fallback. The live codec is shown at the top of this tab.");
            return TRUE;
        }
        if (id == IDC_PRESET_APPLY) {
            int selected = ComboBox_GetCurSel(GetDlgItem(page, IDC_PRESET));
            if (selected <= 0) return TRUE;
            pn::Preset preset = (pn::Preset)(selected - 1);
            PNA2DP_CONFIG c; CollectConfig(c);
            pn::ApplyPreset(c, preset);
            m_s.config = c; LoadAll(); SetDirty(true);
            SetStatusText(std::wstring(L"Preset loaded: ") + pn::PresetName(preset) + L". Press Apply to use it.");
            return TRUE;
        }
    }
    if (page == m_page[PG_SINK]) {
        if (id == IDC_SINK_REFRESH) { m_s.Refresh(true); LoadSink(); LoadCodecs(); return TRUE; }
        return FALSE;
    }
    if (page == m_page[PG_DRIVER]) {
        const bool selected = m_sel >= 0 && m_sel < (int)m_devices.size();
        const DeviceEntry device = selected ? m_devices[m_sel] : DeviceEntry{};
        const DeviceEntry *e = selected ? &device : nullptr;
        switch (id) {
        case IDC_DRV_REQUIREMENTS: {
            LoadDriver();
            Prompt(m_readiness.detail + L"\n\nThese checks do not change Windows security settings. A green light means a PN development-driver requirement is met, not that Windows security is stronger.");
            return TRUE;
        }
        case IDC_DRV_SECURITY:
            if ((INT_PTR)ShellExecuteW(m_hwnd, L"open", L"windowsdefender://coreisolation", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
                Prompt(L"Open Windows Security > Device security > Core isolation to inspect Memory Integrity.");
            return TRUE;
        case IDC_DRV_BOOT_MODE: {
            if (m_demo || pn::IsElevated()) { ModalScope modal(*this); ShowBootModeDialog(g_inst, m_hwnd, m_demo); LoadDriver(); return TRUE; }
            // Boot settings may stay open for minutes. Do not block the audio panel
            // in RunElevated's synchronous wait while the separate dialog is open.
            if (HWND existing = FindWindowW(L"#32770", L"PN-A2DP - Windows boot mode")) {
                ShowWindow(existing, SW_RESTORE); SetForegroundWindow(existing); return TRUE;
            }
            std::wstring exe = pn::ModulePath();
            SHELLEXECUTEINFOW launch{sizeof(launch)};
            launch.fMask = SEE_MASK_FLAG_NO_UI;
            launch.hwnd = m_hwnd; launch.lpVerb = L"runas"; launch.lpFile = exe.c_str();
            launch.lpParameters = L"--op boot-settings"; launch.nShow = SW_SHOWNORMAL;
            if (!ShellExecuteExW(&launch)) {
                DWORD error = GetLastError();
                if (error != ERROR_CANCELLED) Error(L"Cannot open Windows boot settings.", error);
                SetStatusText(error == ERROR_CANCELLED ? L"Boot settings cancelled; no changes made." : L"Cannot open Windows boot settings.");
            } else SetStatusText(L"Windows boot settings opened in a separate administrator window.");
            return TRUE;
        }
        case IDC_DRV_USE_PNA2DP:
            if (!e) return TRUE;
            if (e->demo) { Prompt(L"Demo mode: this would stage pna2dp.inf (if needed) and force it onto the selected device node through SetupAPI (DiInstallDevice with DIIRFLAG_FORCE_INF)."); return TRUE; }
            LoadReadiness();
            if (!m_readiness.ready()) { Prompt(m_readiness.detail, MB_ICONWARNING); return TRUE; }
            if (Prompt(L"Use the newest available PN-A2DP driver for \"" + e->node.name + L"\"?\n\nThe audio endpoint is recreated; a stream that is playing stops for a moment. Windows may require a restart to finish the change.", MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            DriverOp(L"Switching to the PN-A2DP driver", L"use-pna2dp " + Quote(e->node.instanceId), true);
            return TRUE;
        case IDC_DRV_USE_INBOX:
            if (!e) return TRUE;
            if (BootSettingsWindow()) { Prompt(L"Close Windows / Test Mode before switching drivers."); return TRUE; }
            if (e->demo) { Prompt(L"Demo mode: this would select microsoft_bluetooth_a2dp_src.inf from the compatible driver list for the device node and install it."); return TRUE; }
            if (Prompt(L"Switch \"" + e->node.name + L"\" back to the Microsoft inbox driver?\n\nThe PN-A2DP settings for this headset stay in the registry and are used again when you switch back.", MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            DriverOp(L"Restoring the Microsoft driver", L"use-inbox " + Quote(e->node.instanceId), true);
            return TRUE;
        case IDC_DRV_RESCAN:
            DriverOp(L"Rescanning devices", L"rescan", true);
            return TRUE;
        case IDC_DRV_STAGE: {
            wchar_t buf[MAX_PATH] = L"pna2dp.inf";
            OPENFILENAMEW ofn = { sizeof(ofn) };
            ofn.hwndOwner = m_hwnd; ofn.lpstrFilter = L"Driver package (pna2dp.inf)\0pna2dp.inf\0INF files\0*.inf\0"; ofn.lpstrFile = buf; ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST; ofn.lpstrTitle = L"Select the PN-A2DP driver package (pna2dp.inf with pna2dp.sys and pna2dp.cat beside it)";
            std::wstring def = FindPackageInf();
            if (!def.empty()) wcscpy_s(buf, def.c_str());
            { ModalScope modal(*this); if (!GetOpenFileNameW(&ofn)) return TRUE; }
            DriverOp(L"Staging the driver package", L"stage " + Quote(buf), false);
            return TRUE;
        }
        case IDC_DRV_UNSTAGE: {
            auto staged = pn::FindStagedPackages();
            if (staged.empty()) return TRUE;
            if (Prompt(L"Remove " + staged.back().publishedName + L" from the driver store?\n\nHeadsets that use PN-A2DP fall back to the Microsoft driver on their next connection.", MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            DriverOp(L"Removing the driver package", L"unstage " + staged.back().publishedName, true);
            return TRUE;
        }
        case IDC_DRV_SVC_START: DriverOp(L"Starting the codec host service", L"svc-start", false); return TRUE;
        case IDC_DRV_SVC_STOP: DriverOp(L"Stopping the codec host service", L"svc-stop", false); return TRUE;
        case IDC_DRV_SVC_INSTALL: {
            std::wstring exe = pn::JoinPath(GetText(page, IDC_DRV_INSTALLDIR), L"pna2dp-codechost.exe");
            DriverOp(L"Installing the codec host service", L"svc-install " + Quote(exe), false);
            return TRUE;
        }
        case IDC_DRV_SVC_REMOVE:
            if (Prompt(L"Remove the codec host service? Only SBC remains available until it is installed again.", MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            DriverOp(L"Removing the codec host service", L"svc-remove", false);
            return TRUE;
        case IDC_DRV_OPENDIR: ShellExecuteW(m_hwnd, L"open", GetText(page, IDC_DRV_INSTALLDIR).c_str(), NULL, NULL, SW_SHOWNORMAL); return TRUE;
        }
        return FALSE;
    }
    /* generic dirty tracking for the configuration pages */
    if (m_loading) return FALSE;
    bool changed = false;
    if (code == BN_CLICKED || code == CBN_SELCHANGE) changed = true;
    if (code == EN_CHANGE) changed = true;
    if (!changed) return FALSE;
    if (code == BN_CLICKED) SyncAutoGroups(page, id);
    if (page == m_page[PG_ADAPTER] && id == IDC_AD_AUTO && Checked(page, IDC_AD_AUTO)) {
        Check(m_page[PG_TRANSPORT], IDC_TR_ABR, true);
        Check(m_page[PG_LDAC], IDC_LDAC_ABR, true);
    }
    if ((page == m_page[PG_TRANSPORT] && id == IDC_TR_ABR && !Checked(page, IDC_TR_ABR)) ||
        (page == m_page[PG_LDAC] && id == IDC_LDAC_ABR && !Checked(page, IDC_LDAC_ABR)))
        Check(m_page[PG_ADAPTER], IDC_AD_AUTO, false);
    if (page == m_page[PG_SBC]) {
        if (id == IDC_SBC_MAX_SINK) { bool sink = Checked(page, IDC_SBC_MAX_SINK); EnableItem(page, IDC_SBC_MAX_SLIDER, !sink); EnableItem(page, IDC_SBC_MAX_EDIT, !sink); EnableItem(page, IDC_SBC_MAX_SPIN, !sink); }
        if (id == IDC_SBC_MAX_EDIT && code == EN_CHANGE) SendMessageW(GetDlgItem(page, IDC_SBC_MAX_SLIDER), TBM_SETPOS, TRUE, min(250u, max(2u, GetUInt(page, IDC_SBC_MAX_EDIT))));
        UpdateSbcBitrate();
    }
    if (page == m_page[PG_AAC] && id == IDC_AAC_SINKMAX) EnableItem(page, IDC_AAC_BITRATE, !Checked(page, IDC_AAC_SINKMAX));
    if (page == m_page[PG_LDAC]) UpdateLdacBitrate();
    if (page == m_page[PG_TRANSPORT]) {
        if (id == IDC_TR_TARGET_EDIT && code == EN_CHANGE) SendMessageW(GetDlgItem(page, IDC_TR_TARGET_SLIDER), TBM_SETPOS, TRUE, GetUInt(page, IDC_TR_TARGET_EDIT));
        if (id == IDC_TR_MAX_EDIT && code == EN_CHANGE) SendMessageW(GetDlgItem(page, IDC_TR_MAX_SLIDER), TBM_SETPOS, TRUE, GetUInt(page, IDC_TR_MAX_EDIT));
    }
    SetDirty(true);
    return TRUE;
}

INT_PTR Panel::OnPageNotify(HWND page, NMHDR *nm)
{
    if (page == m_page[PG_CODECS] && nm->idFrom == IDC_CODEC_LIST && nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if (!m_loading && (lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK)) {
            /* SBC cannot be disabled: restore the box and treat it as no change */
            if ((ULONG)ListViewParam(nm->hwndFrom, lv->iItem) == PNA2DP_CODEC_SBC && !ListView_GetCheckState(nm->hwndFrom, lv->iItem)) { m_loading = true; ListView_SetCheckState(nm->hwndFrom, lv->iItem, TRUE); m_loading = false; return TRUE; }
            /* keep the edited mask in the model so a list rebuild (headset refresh, caps event) does not lose it */
            SaveCodecs(m_s.config);
            SetDirty(true);
        }
        if (!m_loading && (lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)) UpdateCodecLive();
        return TRUE;
    }
    if (page == m_page[PG_SINK] && nm->idFrom == IDC_SINK_LIST && nm->code == LVN_ITEMCHANGED) {
        NMLISTVIEW *lv = (NMLISTVIEW *)nm;
        if ((lv->uChanged & LVIF_STATE) && (lv->uNewState & LVIS_SELECTED)) {
            ULONG i = (ULONG)ListViewParam(nm->hwndFrom, lv->iItem);
            if (i < m_s.caps.SepCount) SetText(page, IDC_SINK_DETAIL, pn::DescribeSep(m_s.caps.Seps[i]));
        }
        return TRUE;
    }
    if (nm->code == UDN_DELTAPOS) return FALSE;
    return FALSE;
}

INT_PTR Panel::OnPageScroll(HWND page, HWND ctl)
{
    if (m_inPrompt) return TRUE;
    if (!ctl || (m_loading && page != m_page[PG_WINDOWS])) return FALSE;
    int id = GetDlgCtrlID(ctl);
    int pos = (int)SendMessageW(ctl, TBM_GETPOS, 0, 0);
    bool guard = m_loading; m_loading = true;
    if (page == m_page[PG_WINDOWS] && id == IDC_WIN_VOLUME) {
        m_loading = guard;
        if (!WindowsMode() || m_demo) return TRUE;
        m_adjustingVolume = GetCapture() == ctl;
        const unsigned generation = m_sessionGen, revision = ++m_audioRevision;
        HRESULT hr = pn::SetWindowsVolume(m_devices[m_sel].node.instanceId, m_audio.endpointId, (float)pos / 100.0f);
        if (generation != m_sessionGen || revision != m_audioRevision) return TRUE;
        SetTextF(page, IDC_WIN_LEVEL, L"%d%%", pos);
        if (FAILED(hr)) { m_adjustingVolume = false; LoadWindowsAudio(); SetStatusText(L"Volume was not changed. Refresh the selected output and retry."); }
        return TRUE;
    }
    if (page == m_page[PG_SBC] && id == IDC_SBC_MAX_SLIDER) { SetUInt(page, IDC_SBC_MAX_EDIT, pos); UpdateSbcBitrate(); }
    else if (page == m_page[PG_TRANSPORT] && id == IDC_TR_TARGET_SLIDER) SetUInt(page, IDC_TR_TARGET_EDIT, pos);
    else if (page == m_page[PG_TRANSPORT] && id == IDC_TR_MAX_SLIDER) SetUInt(page, IDC_TR_MAX_EDIT, pos);
    else { m_loading = guard; return FALSE; }
    m_loading = guard;
    SetDirty(true);
    return TRUE;
}

void Panel::DriverOp(const std::wstring &what, const std::wstring &args, bool rescanAfter)
{
    if (rescanAfter && m_dirty && m_s.open) {
        int answer = Prompt(L"Apply your changed settings before this driver operation?\n\nYes saves them, No discards them, and Cancel keeps editing.", MB_ICONQUESTION | MB_YESNOCANCEL);
        if (answer == IDCANCEL || (answer == IDYES && !Apply())) return;
    }
    if (m_demo) {
        Prompt(L"Demo mode: " + what + L" is simulated. No device, driver package or Windows service was changed.");
        return;
    }
    SetStatusText(what + L"...");
    SetCursor(LoadCursorW(NULL, IDC_WAIT));
    if (rescanAfter) CloseSession();
    DWORD err;
    {
        ModalScope modal(*this);
        err = Elevated(m_hwnd, args);
    }
    SetCursor(LoadCursorW(NULL, IDC_ARROW));
    if (err == ERROR_CANCELLED) { if (rescanAfter) RefreshDevices(true); SetStatusText(what + L": cancelled at the elevation prompt."); return; }
    if (err == ERROR_REQUEST_ABORTED) { if (rescanAfter) RefreshDevices(true); LoadDriver(); SetStatusText(L"Activation blocked by the requirements check; device driver unchanged."); return; }
    if (err == ERROR_SUCCESS_REBOOT_REQUIRED) { Prompt(what + L" succeeded; Windows asks for a restart to finish the driver change.", MB_ICONINFORMATION); err = ERROR_SUCCESS; }
    if (err != ERROR_SUCCESS) {
        std::wstring hint;
        if (err == ERROR_BUSY) hint = L"\n\nClose Windows / Test Mode and wait for any other driver operation to finish, then retry.";
        if (err == ERROR_FILE_NOT_FOUND) hint = L"\n\nNo pna2dp.inf was found. Use \"Stage package...\" to point at the driver package, or run the installer.";
        if (err == 0xE0000247 || err == 0xE000022F || err == ERROR_DRIVER_BLOCKED || err == 0x800B0109) hint = L"\n\nThe package signature is not trusted. A test-signed build needs test signing enabled (bcdedit /set testsigning on) and the test certificate in the Root and TrustedPublisher stores; see docs/04-build-sign-install.md.";
        Error(what + L" failed." + hint, err);
        SetStatusText(what + L": failed.");
    } else {
        SetStatusText(what + L": done.");
    }
    if (rescanAfter) { Sleep(500); RefreshDevices(true); }
    LoadDriver();
}

/* ------------------------------------------------------------------------- */
/* Entry point                                                                  */
/* ------------------------------------------------------------------------- */

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR, int)
{
    g_inst = inst;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool demo = false;
    int startPage = -1;
    for (int i = 1; i < argc; i++) {
        if (!wcscmp(argv[i], L"--op")) return (int)RunOperation(argc - i - 1, argv + i + 1);
        if (!wcscmp(argv[i], L"--demo")) demo = true;
        if (!wcscmp(argv[i], L"--boot-preview")) return ShowBootModeDialog(inst, nullptr, true);
        if (!wcscmp(argv[i], L"--page") && i + 1 < argc) {
            for (int p = 0; p < PG_COUNT; p++) if (!_wcsicmp(argv[i + 1], kPages[p].title)) startPage = p;
            if (iswdigit(argv[i + 1][0])) startPage = _wtoi(argv[i + 1]);
            i++;
        }
    }
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES | ICC_UPDOWN_CLASS };
    InitCommonControlsEx(&icc);
    // Keep COM initialized across audio refreshes and UI message dispatch.
    struct Runtime {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ~Runtime() { if (SUCCEEDED(hr)) CoUninitialize(); }
    } runtime;
    Panel panel;
    if (!panel.Create(demo)) { MessageBoxW(NULL, L"Cannot create the main window.", L"PN-A2DP", MB_ICONERROR); return 1; }
    if (startPage >= 0) panel.ShowPage(startPage);
    return panel.Run();
}
