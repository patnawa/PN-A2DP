/*
 * plugin_test.cpp - loads every pna2dp-codec-*.dll in a directory, opens an encoder for
 * each codec it offers with representative A2DP parameters, encodes two seconds of a
 * test tone and reports throughput, output size and bitrate. Validates the plugin ABI
 * and the third-party library builds without any Bluetooth hardware.
 *
 *   plugin_test.exe [directory]   (default: the executable's directory)
 *
 * SPDX-License-Identifier: MIT
 */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <vector>
#include <string>
#include "pna2dp_codec_plugin.h"

static int failures = 0;

static void FillParams(PNA2DP_STREAM_PARAMS &p, uint32_t codec)
{
    memset(&p, 0, sizeof(p));
    p.Codec = codec;
    p.Channels = 2;
    p.ChannelMode = PNA2DP_CHMODE_STEREO;
    p.MediaMtu = 1024 - 13;
    switch (codec) {
    case PNA2DP_CODEC_SBC:
        p.SampleRate = 48000; p.PcmFormat = PNA2DP_FMT_S16; p.ChannelMode = PNA2DP_CHMODE_JOINT; p.QualityIndex = 53;
        p.CodecInfo[0] = 0x10 | 0x01; p.CodecInfo[1] = 0x10 | 0x04 | 0x01; p.CodecInfo[2] = 2; p.CodecInfo[3] = 53; p.CodecInfoLength = 4; break;
    case PNA2DP_CODEC_AAC:
        p.SampleRate = 44100; p.PcmFormat = PNA2DP_FMT_S16; p.BitrateBps = 256000;
        p.CodecInfo[0] = 0x80; p.CodecInfo[1] = 0x01; p.CodecInfo[2] = 0x84; p.CodecInfo[3] = 0x83; p.CodecInfo[4] = 0xE8; p.CodecInfo[5] = 0x00; p.CodecInfoLength = 6; break;
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
        p.SampleRate = 48000; p.PcmFormat = PNA2DP_FMT_S24; p.CodecInfoLength = 7; break;
    case PNA2DP_CODEC_APTX_HD:
        p.SampleRate = 48000; p.PcmFormat = PNA2DP_FMT_S24; p.CodecInfoLength = 11; break;
    case PNA2DP_CODEC_LDAC:
        p.SampleRate = 96000; p.PcmFormat = PNA2DP_FMT_S32; p.QualityIndex = 0; p.BitrateBps = 990000; p.CodecInfoLength = 8; break;
    default:
        p.SampleRate = 48000; p.PcmFormat = PNA2DP_FMT_S16; break;
    }
}

static uint32_t BytesPerSample(uint32_t fmt) { return fmt == PNA2DP_FMT_S16 ? 2 : fmt == PNA2DP_FMT_S24 ? 3 : 4; }

static void TestCodec(const PnCodecPlugin *api, uint32_t codec, uint32_t framesPerBlock)
{
    PNA2DP_STREAM_PARAMS p;
    FillParams(p, codec);
    PnCodecEncoder *e = api->open(&p);
    if (!e) { printf("  codec %u: open FAILED\n", codec); failures++; return; }
    uint32_t bps = BytesPerSample(p.PcmFormat);
    std::vector<uint8_t> pcm((size_t)framesPerBlock * p.Channels * bps);
    uint32_t cap = api->max_output_bytes ? api->max_output_bytes(&p) : 4096;
    if (cap < 4096) cap = 4096;
    std::vector<uint8_t> out(cap);
    uint64_t totalOut = 0, totalFrames = 0, packets = 0;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    double phase = 0;
    uint32_t seconds = 2;
    uint32_t blocks = seconds * p.SampleRate / framesPerBlock;
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < framesPerBlock; i++) {
            double v = 0.4 * sin(phase) + 0.2 * sin(phase * 3.7); phase += 2 * 3.14159265 * 440.0 / p.SampleRate;
            for (uint32_t c = 0; c < p.Channels; c++) {
                uint8_t *d = &pcm[((size_t)i * p.Channels + c) * bps];
                int32_t s32 = (int32_t)(v * 2147483647.0);
                if (bps == 2) { int16_t s = (int16_t)(s32 >> 16); memcpy(d, &s, 2); }
                else if (bps == 3) { uint32_t u = (uint32_t)s32 >> 8; d[0] = (uint8_t)u; d[1] = (uint8_t)(u >> 8); d[2] = (uint8_t)(u >> 16); }
                else if (p.PcmFormat == PNA2DP_FMT_F32) { float fv = (float)v; memcpy(d, &fv, 4); }
                else memcpy(d, &s32, 4);
            }
        }
        PnCodecOutput o = {};
        o.buffer = out.data(); o.capacity = (uint32_t)out.size();
        int rc = api->encode(e, pcm.data(), (uint32_t)pcm.size(), framesPerBlock, &o);
        if (rc != 0) { printf("  codec %u: encode FAILED (%d) at block %u\n", codec, rc, b); failures++; break; }
        totalOut += o.written; totalFrames += o.pcm_frames_consumed ? o.pcm_frames_consumed : framesPerBlock; if (o.written) packets++;
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    double audioSecs = (double)totalFrames / p.SampleRate;
    printf("  codec %u: %llu packets, %llu bytes, %.0f kbps, %.1fx realtime\n", codec, packets, totalOut, audioSecs > 0 ? totalOut * 8 / audioSecs / 1000 : 0, audioSecs / (secs + 1e-9));
    if (totalOut == 0) { printf("  codec %u: produced no output\n", codec); failures++; }
    api->close(e);
}

int main(int argc, char **argv)
{
    wchar_t dir[MAX_PATH];
    if (argc > 1) { wchar_t rel[MAX_PATH]; MultiByteToWideChar(CP_ACP, 0, argv[1], -1, rel, MAX_PATH); GetFullPathNameW(rel, MAX_PATH, dir, NULL); }
    else { GetModuleFileNameW(NULL, dir, MAX_PATH); wchar_t *s = wcsrchr(dir, L'\\'); if (s) *s = 0; }
    std::wstring pattern = std::wstring(dir) + L"\\pna2dp-codec-*.dll";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) { printf("no plugins in %ls\n", dir); return 1; }
    do {
        std::wstring path = std::wstring(dir) + L"\\" + fd.cFileName;
        HMODULE m = LoadLibraryExW(path.c_str(), NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) { printf("%ls: load failed %lu\n", fd.cFileName, GetLastError()); failures++; continue; }
        PnCodecPluginGetInfoFn fn = (PnCodecPluginGetInfoFn)GetProcAddress(m, PNA2DP_PLUGIN_ENTRY);
        const PnCodecPlugin *api = fn ? fn() : nullptr;
        if (!api) { printf("%ls: no plugin entry\n", fd.cFileName); failures++; continue; }
        printf("%ls: %s %s (%s) codecs 0x%08x\n", fd.cFileName, api->name, api->version, api->license, api->codec_mask);
        for (uint32_t c = 1; c < PNA2DP_CODEC_MAX; c++) {
            if (!(api->codec_mask & PNA2DP_CODEC_BIT(c))) continue;
            uint32_t frames = (c == PNA2DP_CODEC_AAC) ? 1024 : (c == PNA2DP_CODEC_LDAC) ? 512 : (c == PNA2DP_CODEC_SBC) ? 640 : 512;
            TestCodec(api, c, frames);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
