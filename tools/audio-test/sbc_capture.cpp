/* Explicit, one-packet encoder diagnostic. No continuous recording.
 * SPDX-License-Identifier: MIT */
#include "../common/pnctl.h"
#include <cstdio>
#include <cstdlib>
#include <memory>

int main(int argc, char **argv)
{
    if (argc != 3) { puts("Usage: sbc_capture <device index> <output prefix>; captures one packet of an already playing SBC stream."); return 2; }
    char *end = nullptr; unsigned long index = strtoul(argv[1], &end, 10);
    auto devices = pn::EnumerateControlInterfaces();
    if (end == argv[1] || *end || index >= devices.size()) return 2;
    HANDLE h = CreateFileW(devices[index].path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 1;
    ULONG action = 1; DWORD got = 0;
    bool ok = DeviceIoControl(h, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0, &got, nullptr) != FALSE;
    auto sample = std::make_unique<PNA2DP_SBC_SAMPLE>();
    if (ok) {
        action = 0;
        ULONGLONG deadline = GetTickCount64() + 5000;
        do {
            ok = DeviceIoControl(h, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), sample.get(), sizeof(*sample), &got, nullptr) != FALSE;
            if (ok || GetLastError() != ERROR_NOT_READY) break;
            Sleep(5);
        } while (GetTickCount64() < deadline);
    }
    DWORD error = ok ? 0 : GetLastError();
    if (!ok) {
        action = 2; // disarm even after a timeout; never capture later playback
        DeviceIoControl(h, IOCTL_PNA2DP_SBC_SAMPLE, &action, sizeof(action), nullptr, 0, &got, nullptr);
    }
    CloseHandle(h);
    if (!ok) {
        printf("Capture failed: %lu\n", error);
        if (error == ERROR_ACCESS_DENIED) printf("SBC capture requires an elevated (administrator) prompt\n");
        return 1;
    }
    if (got != sizeof(*sample) || sample->Size != sizeof(*sample) || !sample->PcmBytes || !sample->SbcBytes ||
        sample->PcmBytes > sizeof(sample->Pcm) || sample->SbcBytes > sizeof(sample->Sbc) ||
        !sample->Channels || sample->Channels > 2 || sample->PcmBytes % (sample->Channels * 2)) return 1;
    auto save = [&](const char *suffix, const void *data, size_t bytes) {
        std::string path = std::string(argv[2]) + suffix;
        FILE *f = nullptr;
        if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
        bool written = fwrite(data, 1, bytes, f) == bytes;
        return fclose(f) == 0 && written;
    };
    printf("%lu Hz, %lu channels, gain %lu/32768, muted %lu; PCM %lu bytes, SBC %lu bytes\n", sample->SampleRate, sample->Channels,
        sample->GainQ15, sample->Muted, sample->PcmBytes, sample->SbcBytes);
    return save(".pcm", sample->Pcm, sample->PcmBytes) && save(".sbc", sample->Sbc, sample->SbcBytes) ? 0 : 1;
}
