/* Explicit-device regression tests. --pins requires other audio clients to be closed.
 * Never starts playback or changes the selected endpoint. Run it both unelevated and
 * elevated: trace and audio capture need an elevated caller, and the codec host role
 * belongs to the LocalSystem service only. SPDX-License-Identifier: MIT */
#include "../common/pnctl.h"
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <ksproxy.h>
#include <cstdio>
#include <cstring>
#include <vector>

struct Handle { HANDLE value = INVALID_HANDLE_VALUE; ~Handle() { if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value); } };
static unsigned failures = 0, checks = 0;
static bool Check(bool ok, const char *what) { ++checks; printf("%s: %s (error %lu)\n", ok ? "PASS" : "FAIL", what, ok ? 0 : GetLastError()); if (!ok) ++failures; return ok; }
// Matches the driver's role check: BUILTIN\Administrators enabled (not UAC deny-only).
static bool IsElevatedAdministrator()
{
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD size = sizeof(sid); BOOL member = FALSE;
    return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &size) && CheckTokenMembership(nullptr, sid, &member) && member;
}
static bool IsLocalSystem()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    union { TOKEN_USER user; BYTE bytes[sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE]; } buffer; DWORD size = 0;
    bool system = GetTokenInformation(token, TokenUser, &buffer, sizeof(buffer), &size) && IsWellKnownSid(buffer.user.User.Sid, WinLocalSystemSid);
    CloseHandle(token);
    return system;
}
static bool Ioctl(HANDLE h, DWORD code, void *in, DWORD inSize, void *out, DWORD outSize, DWORD *bytes = nullptr)
{
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, code, in, inSize, out, outSize, &got, nullptr);
    if (bytes) *bytes = got;
    return ok != FALSE;
}

int main(int argc, char **argv)
{
    if (argc < 3 || argc > 4 || strcmp(argv[1], "--device") || (argc > 3 && strcmp(argv[3], "--pins"))) {
        puts("Usage: ks_test --device <index> [--pins]. Close audio clients before --pins; no playback is started."); return 2;
    }
    char *end = nullptr;
    unsigned long index = strtoul(argv[2], &end, 10);
    auto devices = pn::EnumerateControlInterfaces();
    if (end == argv[2] || *end || index >= devices.size()) { puts("No such PN-A2DP device index."); return 2; }
    const auto &device = devices[index];
    Handle control, reader;
    control.value = CreateFileW(device.path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    reader.value = CreateFileW(device.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!Check(control.value != INVALID_HANDLE_VALUE && reader.value != INVALID_HANDLE_VALUE, "normal-user control handles")) return 1;
    const bool system = IsLocalSystem(), elevated = system || IsElevatedAdministrator();
    printf("Caller: %s\n", system ? "LocalSystem" : elevated ? "elevated administrator" : "standard user (run elevated too)");
    // Only the LocalSystem codec host service may register. The probe can never register
    // anything: its Size is 0, which every driver refuses. A driver with control roles
    // refuses it before looking at it (ACCESS_DENIED); one that predates them (0.1.0.18
    // and older) parses it (INVALID_PARAMETER), and the role checks below are skipped for
    // it instead of failing. Never attempted as LocalSystem, where the role is allowed.
    bool roles = true;
    if (!system) {
        PNA2DP_CODEC_REGISTER reg = {};
        reg.Size = 0; reg.AbiVersion = PNA2DP_ABI_VERSION; reg.CodecMask = PNA2DP_CODEC_BIT(PNA2DP_CODEC_SBC);
        reg.MaxPcmBytes = 65536; reg.MaxEncodedBytes = 8192; reg.HostPid = GetCurrentProcessId();
        bool registered = Ioctl(control.value, IOCTL_PNA2DP_CODEC_REGISTER, &reg, sizeof(reg), nullptr, 0);
        DWORD error = GetLastError();
        if (registered) Ioctl(control.value, IOCTL_PNA2DP_CODEC_UNREGISTER, nullptr, 0, nullptr, 0);
        SetLastError(error);
        if (!registered && error == ERROR_INVALID_PARAMETER) {
            roles = false;
            puts("SKIP: the driver predates the control roles (0.1.0.18 or older): role checks skipped");
        } else {
            Check(!registered && error == ERROR_ACCESS_DENIED, "only the LocalSystem codec host can register");
        }
    }
    PNA2DP_TRACE trace = {}; DWORD bytes = 0;
    ULONG action = 2;
    if (!elevated) {
        if (roles) {
            Check(!Ioctl(reader.value, IOCTL_PNA2DP_GET_TRACE, nullptr, 0, &trace, sizeof(trace), &bytes) && GetLastError() == ERROR_ACCESS_DENIED,
                  "standard user cannot read the trace");
            Check(!Ioctl(control.value, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0) && GetLastError() == ERROR_ACCESS_DENIED,
                  "standard user cannot capture audio");
        }
    } else if (Check(Ioctl(reader.value, IOCTL_PNA2DP_GET_TRACE, nullptr, 0, &trace, sizeof(trace), &bytes), "read-only trace snapshot")) {
        bool valid = bytes == sizeof(trace) && trace.Size == sizeof(trace) && trace.Count <= PNA2DP_TRACE_ENTRIES;
        for (ULONG i = 0; valid && i < trace.Count; ++i) {
            valid = memchr(trace.Entries[i].Text, 0, sizeof(trace.Entries[i].Text)) != nullptr;
            if (i) valid = valid && (LONG)(trace.Entries[i].Sequence - trace.Entries[i - 1].Sequence) > 0;
        }
        Check(valid, "bounded, terminated, ordered trace entries");
    }
    if (elevated) {
        Check(!Ioctl(reader.value, IOCTL_PNA2DP_GET_TRACE, nullptr, 0, &trace, sizeof(trace) - 1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER, "short trace output rejected");
    }
    Check(!Ioctl(reader.value, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0) && GetLastError() == ERROR_ACCESS_DENIED,
          "read-only handle cannot capture audio");
    if (elevated) {
        Check(Ioctl(control.value, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0), "diagnostic capture cancellation");
        action = 3;
        Check(!Ioctl(control.value, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0) && GetLastError() == ERROR_INVALID_PARAMETER,
              "invalid capture action rejected");
        action = 0;
        Check(!Ioctl(control.value, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
              "short capture output rejected");
    }
    PNA2DP_CONFIG config = {};
    if (Check(Ioctl(control.value, IOCTL_PNA2DP_GET_CONFIG, nullptr, 0, &config, sizeof(config)), "read settings")) {
        Check(!Ioctl(reader.value, IOCTL_PNA2DP_SET_CONFIG, &config, sizeof(config), nullptr, 0) && GetLastError() == ERROR_ACCESS_DENIED, "read-only handle cannot change settings");
        Check(Ioctl(control.value, IOCTL_PNA2DP_SET_CONFIG, &config, sizeof(config), nullptr, 0), "save unchanged settings without administrator rights");
    }
    // PortCls interfaces use the same device instance with the AUDIO category and Wave reference.
    size_t category = device.path.rfind(L"#{");
    if (category == std::wstring::npos) return 1;
    {
        // This published interface is the Windows Bluetooth volume bridge.
        std::wstring topoPath = device.path.substr(0, category) + L"#{e630310c-1733-42bb-9847-7e45d1a27490}\\Topology";
        Handle topo;
        topo.value = CreateFileW(topoPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (Check(topo.value != INVALID_HANDLE_VALUE, "open Bluetooth volume topology interface")) {
            const GUID volumeSet = {0x401e566b, 0xb47e, 0x4e34, {0xb0,0xfc,0x94,0x82,0x6e,0x9e,0x2a,0x1a}};
            KSPROPERTY remote = {volumeSet, 1, KSPROPERTY_TYPE_GET};
            LONG value = 0;
            Check(Ioctl(topo.value, IOCTL_KS_PROPERTY, &remote, sizeof(remote), &value, sizeof(value), &bytes) && bytes == sizeof(value) && value <= 0 && value >= -4161536,
                  "receiver volume property is bounded 16.16 dB");
            Check(!Ioctl(topo.value, IOCTL_KS_PROPERTY, &remote, sizeof(remote), &value, sizeof(value) - 1) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
                  "short receiver volume output rejected");
            remote.Flags = KSPROPERTY_TYPE_SET;
            for (LONG invalid : {1L, -4161537L}) {
                Check(!Ioctl(topo.value, IOCTL_KS_PROPERTY, &remote, sizeof(remote), &invalid, sizeof(invalid)) && GetLastError() == ERROR_INVALID_PARAMETER,
                      "out-of-range receiver volume rejected");
            }
            PNA2DP_STATUS before = {}, after = {};
            Ioctl(control.value, IOCTL_PNA2DP_GET_STATUS, nullptr, 0, &before, sizeof(before));
            Handle signal;
            signal.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            KSEVENT event = {volumeSet, 1, KSEVENT_TYPE_ENABLE};
            KSEVENTDATA data = {};
            data.NotificationType = KSEVENTF_EVENT_HANDLE; data.EventHandle.Event = signal.value;
            if (Check(signal.value && Ioctl(topo.value, IOCTL_KS_ENABLE_EVENT, &event, sizeof(event), &data, sizeof(data)), "register additional volume listener")) {
                Check(Ioctl(topo.value, IOCTL_KS_DISABLE_EVENT, &data, sizeof(data), nullptr, 0), "remove additional volume listener");
                Check(Ioctl(control.value, IOCTL_PNA2DP_GET_STATUS, nullptr, 0, &after, sizeof(after)) &&
                    ((before.Flags ^ after.Flags) & PNA2DP_STATUS_FLAG_REMOTE_VOLUME) == 0, "removing test listener preserves Windows receiver-volume mode");
            }
            KSP_PIN pin = {}; pin.Property.Set = KSPROPSETID_Pin; pin.Property.Id = KSPROPERTY_PIN_CATEGORY;
            pin.Property.Flags = KSPROPERTY_TYPE_GET; pin.PinId = 1;
            GUID type = {};
            Check(Ioctl(topo.value, IOCTL_KS_PROPERTY, &pin, sizeof(pin), &type, sizeof(type)) &&
                (IsEqualGUID(type, KSNODETYPE_SPEAKER) || IsEqualGUID(type, KSNODETYPE_HEADPHONES)), "receiver endpoint has a supported physical category");
            printf("Endpoint category: %s\n", IsEqualGUID(type, KSNODETYPE_SPEAKER) ? "speakers" : "headphones");
        }
    }
    std::wstring path = device.path.substr(0, category) + L"#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\Wave";
    Handle wave;
    wave.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (!Check(wave.value != INVALID_HANDLE_VALUE, "open WaveRT filter")) return 1;
    KSP_PIN property = {};
    property.Property.Set = KSPROPSETID_Pin;
    property.Property.Id = KSPROPERTY_PIN_PROPOSEDATAFORMAT;
    property.Property.Flags = KSPROPERTY_TYPE_SET;
    KSDATAFORMAT_WAVEFORMATEXTENSIBLE f = {};
    f.DataFormat.FormatSize = sizeof(f); f.DataFormat.SampleSize = 4;
    f.DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO; f.DataFormat.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    f.DataFormat.Specifier = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    f.WaveFormatExt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.WaveFormatExt.Format.nChannels = 2; f.WaveFormatExt.Format.wBitsPerSample = 16;
    f.WaveFormatExt.Format.nBlockAlign = 4; f.WaveFormatExt.Format.cbSize = 22;
    f.WaveFormatExt.Samples.wValidBitsPerSample = 16; f.WaveFormatExt.dwChannelMask = KSAUDIO_SPEAKER_STEREO;
    f.WaveFormatExt.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
    Check(!Ioctl(wave.value, IOCTL_KS_PROPERTY, &property, sizeof(property), &f, sizeof(KSDATAFORMAT)) && GetLastError() == ERROR_INSUFFICIENT_BUFFER,
          "Windows header-sized format probe requests a larger buffer");
    unsigned formats = 0;
    for (ULONG rate : {44100ul, 48000ul}) {
        f.WaveFormatExt.Format.nSamplesPerSec = rate; f.WaveFormatExt.Format.nAvgBytesPerSec = rate * 4;
        if (!Ioctl(wave.value, IOCTL_KS_PROPERTY, &property, sizeof(property), &f, sizeof(f))) { printf("Format %lu Hz not advertised; skipped.\n", rate); continue; }
        ++formats;
        if (argc < 4) continue;
        std::vector<BYTE> buffer(sizeof(KSPIN_CONNECT) + sizeof(f));
        auto connect = (KSPIN_CONNECT*)buffer.data();
        connect->Interface.Set = KSINTERFACESETID_Standard; connect->Interface.Id = KSINTERFACE_STANDARD_LOOPED_STREAMING;
        connect->Medium.Set = KSMEDIUMSETID_Standard; connect->Medium.Id = KSMEDIUM_TYPE_ANYINSTANCE;
        connect->Priority.PriorityClass = KSPRIORITY_NORMAL; connect->Priority.PrioritySubClass = 1;
        memcpy(connect + 1, &f, sizeof(f));
        Handle pin;
        DWORD error = KsCreatePin(wave.value, connect, GENERIC_READ | GENERIC_WRITE, &pin.value);
        SetLastError(error);
        if (!Check(!error, "create a supported WaveRT pin")) continue;
        KSRTAUDIO_BUFFER_PROPERTY_WITH_NOTIFICATION request = {};
        request.Property.Set = KSPROPSETID_RtAudio; request.Property.Id = KSPROPERTY_RTAUDIO_BUFFER_WITH_NOTIFICATION;
        request.Property.Flags = KSPROPERTY_TYPE_GET; request.RequestedBufferSize = rate * 4 / 50; request.NotificationCount = 2;
        KSRTAUDIO_BUFFER audioBuffer = {};
        if (!Check(Ioctl(pin.value, IOCTL_KS_PROPERTY, &request, sizeof(request), &audioBuffer, sizeof(audioBuffer)), "allocate 20 ms event buffer")) continue;
        KSPROPERTY latencyProperty = {KSPROPSETID_RtAudio, KSPROPERTY_RTAUDIO_HWLATENCY, KSPROPERTY_TYPE_GET};
        KSRTAUDIO_HWLATENCY latency = {};
        if (Check(Ioctl(pin.value, IOCTL_KS_PROPERTY, &latencyProperty, sizeof(latencyProperty), &latency, sizeof(latency)), "read hardware latency")) {
            ULONGLONG lead = latency.FifoSize + (ULONGLONG)(latency.ChipsetDelay + latency.CodecDelay) * rate * 4 / 10000000;
            Check(audioBuffer.ActualBufferSize && lead < audioBuffer.ActualBufferSize, "hardware lead fits the Windows cyclic buffer");
        }
    }
    Check(formats != 0, "at least one standard stereo format accepted");
    printf("%u checks, %u failures; no playback started.\n", checks, failures);
    return failures ? 1 : 0;
}
