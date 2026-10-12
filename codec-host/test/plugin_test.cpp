/*
 * plugin_test.cpp - loads every pna2dp-codec-*.dll in a directory, opens an encoder for
 * each codec it offers with representative A2DP parameters, encodes one second of
 * bass/transients/stereo audio and reports throughput, output size and bitrate. Validates the plugin ABI
 * and the third-party library builds without any Bluetooth hardware.
 *
 *   plugin_test.exe [directory] [vector output directory]
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
static const char *vector_dir = nullptr;

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
        p.SampleRate = 44100; p.PcmFormat = PNA2DP_FMT_S16; p.BitrateBps = 320000;
        p.CodecInfo[0] = 0x80; p.CodecInfo[1] = 0x01; p.CodecInfo[2] = 0x84; p.CodecInfo[3] = 0x84; p.CodecInfo[4] = 0xE2; p.CodecInfo[5] = 0x00; p.CodecInfoLength = 6; break;
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

static void TestCodec(const PnCodecPlugin *api, const PNA2DP_STREAM_PARAMS &p, uint32_t framesPerBlock)
{
    uint32_t codec = p.Codec;
    PnCodecEncoder *e = api->open(&p);
    if (!e) { printf("  codec %u: open FAILED\n", codec); failures++; return; }
    uint32_t bps = BytesPerSample(p.PcmFormat);
    std::vector<uint8_t> pcm((size_t)framesPerBlock * p.Channels * bps);
    uint32_t cap = api->max_output_bytes ? api->max_output_bytes(&p) : 4096;
    if (cap < 4096) cap = 4096;
    std::vector<uint8_t> out(cap + 32, 0xcd);
    FILE *encoded = nullptr, *reference = nullptr;
    if (vector_dir && codec != PNA2DP_CODEC_LDAC) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%u-%u-%u-%u-%u-%u.bin", vector_dir, codec, p.SampleRate, p.Channels, p.PcmFormat, p.MediaMtu, p.QualityIndex);
        fopen_s(&encoded, path, "wb");
        snprintf(path, sizeof(path), "%s/%u-%u-%u-%u-%u-%u.f32", vector_dir, codec, p.SampleRate, p.Channels, p.PcmFormat, p.MediaMtu, p.QualityIndex);
        fopen_s(&reference, path, "wb");
        if (!encoded || !reference) { printf("Cannot create plugin vectors\n"); ++failures; }
    }
    uint64_t totalOut = 0, totalFrames = 0, packets = 0;
    LARGE_INTEGER f, t0, t1; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    uint32_t blocks = p.SampleRate / framesPerBlock;
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < framesPerBlock; i++) {
            uint32_t sample = b * framesPerBlock + i;
            double t = double(sample) / p.SampleRate;
            for (uint32_t c = 0; c < p.Channels; c++) {
                double v;
                if (t < 0.25) v = 0.99 * sin(2 * 3.141592653589793 * 53 * t);
                else if (t < 0.75) {
                    v = sample % 997 < 498 ? 1.0 : -1.0;
                    if (t >= 0.5 && c) v = -v; // stress difference as well as sum
                } else {
                    v = 0.70 * sin(2 * 3.141592653589793 * (c ? 97 : 63) * t) +
                        0.29 * sin(2 * 3.141592653589793 * (c ? 6971 : 3011) * t);
                }
                uint8_t *d = &pcm[((size_t)i * p.Channels + c) * bps];
                int32_t s32 = (int32_t)(v * 2147483647.0);
                if (bps == 2) { int16_t s = (int16_t)(s32 >> 16); memcpy(d, &s, 2); }
                else if (bps == 3) { uint32_t u = (uint32_t)s32 >> 8; d[0] = (uint8_t)u; d[1] = (uint8_t)(u >> 8); d[2] = (uint8_t)(u >> 16); }
                else if (p.PcmFormat == PNA2DP_FMT_F32) { float fv = (float)v; memcpy(d, &fv, 4); }
                else memcpy(d, &s32, 4);
                if (reference) {
                    float value = p.PcmFormat == PNA2DP_FMT_F32 ? (float)v :
                        bps == 2 ? (float)(s32 >> 16) / 32768.0f :
                        bps == 3 ? (float)(s32 >> 8) / 8388608.0f : (float)((double)s32 / 2147483648.0);
                    if (fwrite(&value, sizeof(value), 1, reference) != 1) ++failures;
                }
            }
        }
        PnCodecOutput o = {};
        o.buffer = out.data() + 16; o.capacity = cap;
        int rc = api->encode(e, pcm.data(), (uint32_t)pcm.size(), framesPerBlock, &o);
        if (rc != 0) { printf("  codec %u: encode FAILED (%d) at block %u\n", codec, rc, b); failures++; break; }
        bool valid = o.written <= cap && (codec == PNA2DP_CODEC_AAC || o.written <= p.MediaMtu);
        for (unsigned k = 0; k < 16; ++k) valid = valid && out[k] == 0xcd && out[cap + 16 + k] == 0xcd;
        if (o.written && (!o.frame_count || !o.pcm_frames_consumed)) valid = false;
        if (!valid) { printf("  invalid output metadata/bounds for codec %u\n", codec); ++failures; break; }
        if (encoded && o.written) {
            if (codec == PNA2DP_CODEC_AAC) {
                // Add only LOAS framing so FFmpeg can independently decode the
                // exact LATM AudioMuxElement emitted for Bluetooth.
                uint8_t loas[3] = {0x56, (uint8_t)(0xe0 | (o.written >> 8)), (uint8_t)o.written};
                if (fwrite(loas, 1, 3, encoded) != 3) ++failures;
            }
            if (fwrite(o.buffer, 1, o.written, encoded) != o.written) ++failures;
        }
        totalOut += o.written;
        if (o.written) { totalFrames += o.pcm_frames_consumed; ++packets; }
    }
    QueryPerformanceCounter(&t1);
    double secs = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    double audioSecs = (double)totalFrames / p.SampleRate;
    printf("  codec %u: %u Hz/%u ch/fmt %u/mtu %u/q%u: %llu packets, %llu bytes, %.0f kbps, %.1fx realtime\n", codec, p.SampleRate, p.Channels, p.PcmFormat, p.MediaMtu, p.QualityIndex, packets, totalOut, audioSecs > 0 ? totalOut * 8 / audioSecs / 1000 : 0, audioSecs / (secs + 1e-9));
    if (totalOut == 0) { printf("  codec %u: produced no output\n", codec); failures++; }
    uint64_t inputFrames = uint64_t(blocks) * framesPerBlock;
    uint32_t buffered = codec == PNA2DP_CODEC_LDAC ? 15 * (p.SampleRate >= 88200 ? 256 : 128) : codec == PNA2DP_CODEC_AAC ? 2048 : 0;
    if (totalFrames > inputFrames || totalFrames + buffered < inputFrames) {
        printf("  missing or duplicated PCM: input %llu, represented %llu, allowed buffered %u\n", inputFrames, totalFrames, buffered);
        ++failures;
    }
    if (api->reconfigure && api->reconfigure(e, codec == PNA2DP_CODEC_LDAC ? 2 : p.QualityIndex, 128000) != 0) {
        printf("  live reconfigure failed\n"); ++failures;
    }
    uint64_t afterReconfigure = 0;
    // A mono MQ LDAC packet can buffer twelve frames before emitting. Allow
    // two maximum packet windows, rather than mistaking normal buffering for loss.
    uint64_t afterFrames = 0;
    const unsigned reconfigureBlocks = 32;
    for (unsigned i = 0; i < reconfigureBlocks; ++i) {
        PnCodecOutput next = {};
        next.buffer = out.data() + 16; next.capacity = cap;
        int rc = api->encode(e, pcm.data(), (uint32_t)pcm.size(), framesPerBlock, &next);
        bool valid = !rc && next.written <= cap && (codec == PNA2DP_CODEC_AAC || next.written <= p.MediaMtu);
        for (unsigned k = 0; k < 16; ++k) valid = valid && out[k] == 0xcd && out[cap + 16 + k] == 0xcd;
        if (next.written && (!next.frame_count || !next.pcm_frames_consumed)) valid = false;
        if (!valid) { puts("  encoding after live reconfiguration failed"); ++failures; break; }
        afterReconfigure += next.written;
        afterFrames += next.pcm_frames_consumed;
    }
    if (!afterReconfigure) { puts("  no output after live reconfiguration"); ++failures; }
    uint64_t expectedAfter = inputFrames + uint64_t(reconfigureBlocks) * framesPerBlock;
    if (totalFrames + afterFrames > expectedAfter || totalFrames + afterFrames + buffered < expectedAfter) {
        puts("  missing or duplicated PCM after reconfiguration"); ++failures;
    }
    api->close(e);
    if (encoded && fclose(encoded)) ++failures;
    if (reference && fclose(reference)) ++failures;
}

int main(int argc, char **argv)
{
    if (argc > 2) vector_dir = argv[2];
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
        if (!api || api->api_version != PNA2DP_PLUGIN_API_VERSION || !api->open || !api->encode || !api->close) { printf("%ls: invalid plugin ABI\n", fd.cFileName); failures++; FreeLibrary(m); continue; }
        printf("%ls: %s %s (%s) codecs 0x%08x\n", fd.cFileName, api->name, api->version, api->license, api->codec_mask);
        for (uint32_t c = 1; c < PNA2DP_CODEC_MAX; c++) {
            if (!(api->codec_mask & PNA2DP_CODEC_BIT(c))) continue;
            for (uint32_t rate : {44100u, 48000u, 88200u, 96000u}) {
                if (c != PNA2DP_CODEC_LDAC && c != PNA2DP_CODEC_AAC && rate > 48000) continue;
                for (uint32_t mtu : {337u, 1011u}) {
                    for (uint32_t mode : {PNA2DP_CHMODE_MONO, PNA2DP_CHMODE_STEREO, PNA2DP_CHMODE_DUAL}) {
                        if (mode == PNA2DP_CHMODE_DUAL && c != PNA2DP_CODEC_LDAC) continue;
                        uint32_t channels = mode == PNA2DP_CHMODE_MONO ? 1 : 2;
                        if (channels == 1 && c >= PNA2DP_CODEC_APTX && c <= PNA2DP_CODEC_APTX_LL) continue;
                        for (uint32_t fmt : {PNA2DP_FMT_S16, PNA2DP_FMT_S24, PNA2DP_FMT_S32, PNA2DP_FMT_F32}) {
                            if (c != PNA2DP_CODEC_LDAC && fmt != uint32_t(c == PNA2DP_CODEC_AAC ? PNA2DP_FMT_S16 : PNA2DP_FMT_S24)) continue;
                            for (uint32_t quality = 0; quality < (c == PNA2DP_CODEC_LDAC ? 3u : 1u); ++quality) {
                                PNA2DP_STREAM_PARAMS p;
                                FillParams(p, c);
                                p.SampleRate = rate; p.PcmFormat = fmt; p.Channels = channels;
                                p.ChannelMode = mode;
                                p.MediaMtu = mtu; p.QualityIndex = quality;
                                if (c == PNA2DP_CODEC_LDAC && mtu < 666) p.MediaMtu = 666; // libldac's documented minimum full MTU is 679
                                if (c == PNA2DP_CODEC_AAC && mtu == 337) p.CodecInfo[3] &= 0x7f; // CBR frames require RTP fragmentation
                                uint32_t frames = c == PNA2DP_CODEC_AAC ? 1024 : c == PNA2DP_CODEC_LDAC ? (rate >= 88200 ? 256 : 128) : (mtu / (c == PNA2DP_CODEC_APTX_HD ? 12 : 8)) * 4;
                                TestCodec(api, p, frames);
                            }
                        }
                    }
                }
            }
        }
        FreeLibrary(m);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
