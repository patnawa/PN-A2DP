/*
 * quality_test.cpp - end-to-end audio quality harness for every PN-A2DP codec path.
 *
 * Drives the PRODUCTION code with synthetic signals and measures distortion, clipping,
 * clicks/crackles and gaps after an independent decoder (FFmpeg):
 *
 *   engine PCM (S16 / S24 / 24-in-32 / S32 / F32)
 *     -> WaveRT tick + starvation silence      (wave_math.h rules, emulated tick loop; the 2 ms
 *                                               hold-back FIFO and the gap / resume fades of
 *                                               audio_wave.cpp replicated, fades by pcm.cpp PnPcmRamp)
 *     -> pump PCM ring, latency guard + trim   (audio_policy.h PnTrimPlan / PnEffectiveLatencyMs,
 *                                               ring + PumpTrim, PumpFinishStream (pause) and
 *                                               PumpStop/StartStream (restart) replicated minimally
 *                                               from pump.cpp; SBC start and repacking by the
 *                                               production A2dpSbcPlanStream / A2dpSbcFramesPerPacket)
 *     -> PnConvertPcmEx                        (pcm.cpp: conversion, gain ramps, dither, crossfade, 2:1 decimator)
 *     -> kernel SBC (common/sbc/sbc.c)  or  codec plugin DLL through the plugin ABI
 *        (codec-host/plugins: SBC built in, AAC/aptX/LDAC from out\Release) with the
 *        pump's host ledger (audio_policy.h PnHostLedger* / PnHostResultValid)
 *     -> RTP + media payload header + AAC fragmentation (rtp.cpp RtpWriteHeader; the
 *        SendMediaPacket rules are replicated minimally from pump.cpp, which needs the kernel)
 *     -> captured packets -> receiver checks (RtpParseHeader, sbc_parse_header, LDAC frame walk)
 *     -> FFmpeg decode (sbc, aac via LOAS, aptx, aptx_hd) placed on the RTP timeline
 *     -> analysis against the codec input (delay aligned): SNR, THD+N, level, peak/clip,
 *        channel balance/swap/polarity, IMD, click/crackle detector, gap detector.
 *
 * Codec configuration comes from a2dp_codec.cpp (A2dpParseCaps / A2dpSelectConfig /
 * A2dpQualityAtLevel) for realistic sink capabilities (Sony CMT-SBT40D SBC IE ff ff 02 35).
 * LDAC has no decoder: encoder-side checks only (frames, packet sizes, determinism, drops).
 *
 *   quality_test.exe --plugins <dir with pna2dp-codec-*.dll> --work <dir> [--ffmpeg <exe>]
 *                    [--threads N] [--filter <substring>] [--keep]
 *
 * Writes <work>\results.json and prints a summary. Exit code 0 = no defect flags.
 *
 * Verdicts (constants TH_* below):
 *   CLICK     residual (decoded - gain-matched codec input) above 3 kHz, 1 ms windows, more than
 *             +15 dB over its running median (+-50 ms) and above -96 dBFS; or any wideband burst
 *             > +25 dB. Clean runs of every codec stay <= +9 dB HF; AAC's frame-wise noise
 *             modulation on pure low tones reaches +21 dB wideband but not HF (reported as info).
 *             Steady signals only; transient signals (step, square, impulses) carry legitimate
 *             pre-echo. Events (gain, ABR, trims) are scored against the residual 50..250 ms
 *             before/after and must also be within 40 dB of the programme level.
 *   CLICK(pre-codec)  tone annihilator x[n+1]-2cos(w)x[n]+x[n-1] of the codec input in 1 ms
 *             windows > -40 dB re tone and +15 dB over its floor: a hard cut of a tone measures
 *             -10..-25 dB, the production 4 ms trim crossfade -52..-60 dB, one-block gain ramps
 *             below -59 dB.
 *   GAP       digital-zero run >= 1 ms or a 5 ms window 20 dB under the reference while the
 *             reference is above -50 dBFS (below 15 kHz).
 *   LEVEL     tone level off by > 0.5 dB; CLIP decoded full-scale samples while the codec input
 *             peaks below -0.5 dBFS; CHANNEL swap/polarity/separation < 40 dB/balance > 0.5 dB.
 *   PACKET    RTP version/PT/marker/sequence/timestamp, payload-header frame counts, SBC/LDAC
 *             frame walk, aptX block multiples, MTU, AAC fragmentation; PCM ACCOUNTING compares
 *             the RTP timeline with the codec input.
 *   PAUSE     audio rendered before a KS pause left unencoded once the pump drained.
 *   global    SBC XQ below plain dual channel for the same sink, MTU and radio.
 *   KNOWN     a reachable state with a documented, unfixed limit (the dual-channel SBC ABR floor,
 *             #31) reports its verdicts by name instead of failing; KNOWN ISSUE WORSE fails the
 *             run when it measures worse than recorded (finding #2: not relabelled as stress).
 * Fault groups: volume/mute ramps, latency trims (with and without the crossfade), a starved
 * packet, a lost host block, ABR / bitrate / EQMID walks, a stream restart after a 200 ms outage
 * (the sink starts a new decoder; the onset must not click), a pause / resume (fades) and the
 * pause race (findings #1/#7: 16 pause instants, the pump taking every block it can, 50 ms of
 * receiver silence).
 * Self-tests (group "selftest") inject a dropped, duplicated or corrupted packet and a -40 dBFS
 * one-sample click; each must be reported or the run fails. Baselines run FFmpeg's own SBC and
 * aptX encoders on the same codec input to show what the codec itself does.
 * SPDX-License-Identifier: MIT
 */
#include "codec_core.h"
#include "pcm.h"
#include "audio_policy.h"
#include "wave_math.h"
#include "pna2dp_codec_plugin.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <math.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>
#include <set>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <complex>
#include <functional>
#include <climits>

extern "C" const PnCodecPlugin *PnBuiltinSbcPlugin(void);

static const double PI = 3.14159265358979323846;

/* ------------------------------------------------------------------------- */
/* Thresholds (justification in the report / --help; calibrated on clean runs)  */
/* ------------------------------------------------------------------------- */
/* Residual (decoded - aligned codec input) energy in a 1 ms window against the running
 * median of the surrounding +-50 ms. Stationary codec noise stays within ~+9 dB on every
 * clean codec run; a 1-sample discontinuity of a -6 dBFS tone is > +30 dB. */
static const double TH_CLICK_DB = 15.0;
/* A wideband (unfiltered) residual burst this far over the local median is a defect even
 * when it is not HF-dominant. AAC's frame-wise noise modulation on pure low tones reaches
 * +21 dB on clean runs. */
static const double TH_CLICK_WIDE_DB = 25.0;
/* Residual windows quieter than this (dBFS RMS) are never a click: below audibility. */
static const double TH_CLICK_FLOOR_DBFS = -96.0;
/* Pre-codec discontinuity: RMS of the tone annihilator x[n+1]-2cos(w)x[n]+x[n-1] in a
 * 1 ms window, relative to the tone amplitude. A hard cut of a tone is -17..-25 dB, a 4 ms
 * linear crossfade about -55 dB, a one-block gain ramp -60 dB and below. */
static const double TH_DISC_DB = -40.0;
/* A run of digital zeros (all channels) while the reference is active (> -50 dBFS). */
static const double TH_GAP_MS = 1.0;
/* Level error for tones in band. */
static const double TH_LEVEL_DB = 0.5;

/* ------------------------------------------------------------------------- */
/* Small utilities                                                              */
/* ------------------------------------------------------------------------- */

static std::string Sfmt(const char *f, ...)
{
    char buf[2048];
    va_list ap; va_start(ap, f); vsnprintf(buf, sizeof(buf), f, ap); va_end(ap);
    return buf;
}

static double Db10(double x) { return x > 1e-300 ? 10.0 * log10(x) : -3000.0; }
static double Db20(double x) { return x > 1e-300 ? 20.0 * log10(x) : -3000.0; }

struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t Next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
    double Uni() { return (double)(Next() >> 11) * (1.0 / 9007199254740992.0); }
    double Tpdf() { return Uni() + Uni() - 1.0; }
};

static std::wstring Widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, NULL, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
static std::string Narrow(const std::wstring &w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, NULL, 0, NULL, NULL);
    std::string s(n ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, NULL, NULL);
    return s;
}

static bool WriteFileBytes(const std::string &path, const void *data, size_t n)
{
    FILE *f = NULL;
    if (fopen_s(&f, path.c_str(), "wbN") || !f) return false;
    bool ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}
static bool ReadFileBytes(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = NULL;
    out.clear();
    if (fopen_s(&f, path.c_str(), "rbN") || !f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n > 0) { out.resize((size_t)n); if (fread(out.data(), 1, (size_t)n, f) != (size_t)n) { fclose(f); return false; } }
    fclose(f);
    return true;
}

static std::wstring g_Ffmpeg;
static std::string g_Work;
static bool g_Keep = false;
static std::string Safe(const std::string &s);
static std::mutex g_SpawnMx;   /* the inheritable log handle must not leak into a concurrent child */
static int RunProcess(const std::wstring &cmdline, const std::string &logPath)
{
    std::unique_lock<std::mutex> spawn(g_SpawnMx);
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE log = CreateFileW(Widen(logPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = NULL;
    si.hStdOutput = log;
    si.hStdError = log;
    PROCESS_INFORMATION pi = {};
    std::wstring cmd = cmdline;
    BOOL ok = CreateProcessW(NULL, &cmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    spawn.unlock();
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}

static int Ffmpeg(const std::string &args, const std::string &logPath)
{
    return RunProcess(L"\"" + g_Ffmpeg + L"\" -hide_banner -nostdin -v error -y " + Widen(args), logPath);
}

/* iterative radix-2 FFT */
static void Fft(std::vector<std::complex<double>> &a, bool inverse)
{
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        double ang = 2 * PI / (double)len * (inverse ? 1 : -1);
        std::complex<double> wl(cos(ang), sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1);
            for (size_t j = 0; j < len / 2; j++) {
                std::complex<double> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v; a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    if (inverse) for (auto &x : a) x /= (double)n;
}

/* ------------------------------------------------------------------------- */
/* Signals                                                                       */
/* ------------------------------------------------------------------------- */

enum SigKind { K_TONE, K_SWEEP, K_IMD, K_SQUARE, K_IMPULSE, K_PINK, K_SILENCE, K_STEP, K_LONLY, K_RONLY,
               K_OPPOSITE, K_STEREO2, K_ALIAS, K_NANINF };

struct SigDef {
    std::string name;
    SigKind kind;
    double amp;       /* peak amplitude (pink: RMS) */
    double f0, f1;
    double dur;       /* active seconds; 0.3 s of silence follow */
    bool steady;      /* click verdict applies */
};

static const double kTone = 997.0;

static std::vector<SigDef> CoreSignals()
{
    return {
        { "sine997_0dBFS",  K_TONE, 1.0, kTone, 0, 1.5, true },
        { "sine997_-1dBFS", K_TONE, pow(10, -1 / 20.0), kTone, 0, 1.5, true },
        { "sine997_-6dBFS", K_TONE, pow(10, -6 / 20.0), kTone, 0, 1.5, true },
        { "sine997_-20dBFS", K_TONE, pow(10, -20 / 20.0), kTone, 0, 1.5, true },
        { "sine997_-60dBFS", K_TONE, pow(10, -60 / 20.0), kTone, 0, 1.5, true },
        { "sweep20-20k_-6dBFS", K_SWEEP, 0.5, 20, 20000, 4.0, true },
        { "imd19k+20k", K_IMD, 0.45, 19000, 20000, 1.5, true },
        { "square211_-6dBFS", K_SQUARE, 0.5, 211, 0, 1.5, false },
        { "impulses_23ms", K_IMPULSE, 0.9, 1000.0 / 23.0, 0, 1.5, false },
        { "pink_-20dBFSrms", K_PINK, 0.1, 0, 0, 1.5, true },
        { "silence", K_SILENCE, 0, 0, 0, 1.5, true },
        { "step_silence_to_-1dBFS", K_STEP, pow(10, -1 / 20.0), kTone, 0.5, 1.5, false },
        { "bass60_0dBFS", K_TONE, 1.0, 60, 0, 1.5, true },
        { "L_only", K_LONLY, 0.5, kTone, 0, 1.5, true },
        { "R_only", K_RONLY, 0.5, kTone, 0, 1.5, true },
        { "opposite_phase", K_OPPOSITE, 0.5, kTone, 0, 1.5, true },
        { "L997_R1499", K_STEREO2, 0.5, kTone, 1499, 1.5, true },
    };
}

static SigDef FindSig(const char *name)
{
    for (auto &s : CoreSignals()) if (s.name == name) return s;
    if (!strcmp(name, "naninf")) return { "naninf_f32", K_NANINF, 0.5, kTone, 0, 1.5, false };
    if (!strcmp(name, "overrange")) return { "overrange_1.2_f32", K_TONE, 1.2, kTone, 0, 1.5, true };
    if (!strcmp(name, "tone20k")) return { "sine20k_-6dBFS", K_TONE, 0.5, 20000, 0, 1.5, true };
    if (!strcmp(name, "alias")) return { "alias_above_nyquist", K_ALIAS, 0.5, 30000, 0, 1.5, true };
    if (!strcmp(name, "fault_tone")) return { "sine997_-6dBFS", K_TONE, 0.5, kTone, 0, 2.4, true };
    fprintf(stderr, "unknown signal %s\n", name);
    exit(2);
}

static bool SigHasTone(const SigDef &s)
{
    return s.kind == K_TONE || s.kind == K_LONLY || s.kind == K_RONLY || s.kind == K_OPPOSITE || s.kind == K_NANINF;
}

/* Analytic value (NaN when the signal is not analytic). */
static double SigAnalytic(const SigDef &s, int ch, double t)
{
    if (t < 0 || t >= s.dur) return 0;
    switch (s.kind) {
    case K_TONE: return s.amp * sin(2 * PI * s.f0 * t);
    case K_SWEEP: {
        double k = log(s.f1 / s.f0);
        return s.amp * sin(2 * PI * s.f0 * s.dur / k * (exp(t * k / s.dur) - 1));
    }
    case K_IMD: return s.amp * (sin(2 * PI * s.f0 * t) + sin(2 * PI * s.f1 * t));
    case K_STEP: return t < s.f1 ? 0 : s.amp * cos(2 * PI * s.f0 * (t - s.f1));
    case K_LONLY: return ch == 0 ? s.amp * sin(2 * PI * s.f0 * t) : 0;
    case K_RONLY: return ch == 1 ? s.amp * sin(2 * PI * s.f0 * t) : 0;
    case K_OPPOSITE: return (ch == 0 ? 1 : -1) * s.amp * sin(2 * PI * s.f0 * t);
    case K_STEREO2: return s.amp * sin(2 * PI * (ch == 0 ? s.f0 : s.f1) * t);
    case K_ALIAS: return s.amp * sin(2 * PI * s.f0 * t);
    case K_SILENCE: return 0;
    default: return NAN;
    }
}

/* Engine-rate stereo signal (doubles, may exceed +-1 or be non-finite for float tests). */
static void GenSignal(const SigDef &s, double fs, size_t frames, std::vector<double> &L, std::vector<double> &R)
{
    L.assign(frames, 0); R.assign(frames, 0);
    size_t active = (size_t)(s.dur * fs);
    if (active > frames) active = frames;
    if (s.kind == K_PINK) {
        for (int c = 0; c < 2; c++) {
            Rng r(0x1234 + c * 977);
            double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0;
            std::vector<double> &d = c ? R : L;
            double acc = 0;
            for (size_t n = 0; n < active; n++) {
                double w = r.Uni() * 2 - 1;   /* Paul Kellet's pink filter */
                b0 = 0.99886 * b0 + w * 0.0555179; b1 = 0.99332 * b1 + w * 0.0750759; b2 = 0.96900 * b2 + w * 0.1538520;
                b3 = 0.86650 * b3 + w * 0.3104856; b4 = 0.55000 * b4 + w * 0.5329522; b5 = -0.7616 * b5 - w * 0.0168980;
                double p = b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362; b6 = w * 0.115926;
                d[n] = p; acc += p * p;
            }
            double rms = sqrt(acc / (double)(active ? active : 1));
            for (size_t n = 0; n < active; n++) d[n] *= s.amp / rms;
        }
        return;
    }
    if (s.kind == K_SQUARE) {
        for (size_t n = 0; n < active; n++) { double ph = fmod(s.f0 * n / fs, 1.0); L[n] = R[n] = ph < 0.5 ? s.amp : -s.amp; }
        return;
    }
    if (s.kind == K_IMPULSE) {
        double period = fs / s.f0;
        for (double p = 0.05 * fs; p < (double)active; p += period) { size_t n = (size_t)(p + 0.5); if (n < active) L[n] = R[n] = s.amp; }
        return;
    }
    for (size_t n = 0; n < active; n++) {
        double t = n / fs;
        if (s.kind == K_NANINF) { L[n] = R[n] = s.amp * sin(2 * PI * s.f0 * t); continue; }
        L[n] = SigAnalytic(s, 0, t);
        R[n] = SigAnalytic(s, 1, t);
    }
    if (s.kind == K_NANINF) {
        size_t at[] = { (size_t)(0.40 * fs), (size_t)(0.60 * fs), (size_t)(0.80 * fs), (size_t)(1.00 * fs) };
        if (at[3] + 64 < active) {
            L[at[0]] = NAN;
            R[at[1]] = INFINITY;
            L[at[2]] = -INFINITY;
            for (size_t k = 0; k < 64; k++) L[at[3] + k] = R[at[3] + k] = NAN;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Engine (WaveRT) formats                                                       */
/* ------------------------------------------------------------------------- */

struct EngineFmt { const char *name; ULONG container, valid; bool isFloat; };
static const EngineFmt kFmts[] = {
    { "s16", 16, 16, false }, { "s24", 24, 24, false }, { "s24in32", 32, 24, false }, { "s32", 32, 32, false }, { "f32", 32, 32, true },
};
enum { FMT_S16, FMT_S24, FMT_S24IN32, FMT_S32, FMT_F32 };

/* Quantise to the engine format (TPDF dither for 16/24-bit, as a good source would) and
 * return the value each sample holds, as the production converter is contracted to read it. */
static void EncodeEngine(const std::vector<double> &L, const std::vector<double> &R, const EngineFmt &f,
                         std::vector<uint8_t> &bytes, std::vector<double> &held)
{
    size_t frames = L.size();
    ULONG bps = f.container / 8;
    bytes.assign(frames * 2 * bps, 0);
    held.assign(frames * 2, 0);
    Rng r(0xC0FFEE ^ f.container ^ (f.valid << 8));
    double lastFinite[2] = { 0, 0 };
    unsigned nonFiniteRun[2] = { 0, 0 };
    for (size_t n = 0; n < frames; n++) {
        for (int c = 0; c < 2; c++) {
            double x = c ? R[n] : L[n];
            uint8_t *d = &bytes[(n * 2 + c) * bps];
            if (f.isFloat) {
                float v = (float)x;
                memcpy(d, &v, 4);
                double h;
                if (v != v || v == INFINITY || v == -INFINITY) {
                    /* NaN and Inf: the channel's last finite sample, a run fading to silence over
                     * PN_PCM_NONFINITE_HOLD samples (PnConvertPcmEx, finding #6) */
                    if (nonFiniteRun[c] <= PN_PCM_NONFINITE_HOLD) nonFiniteRun[c]++;
                    h = nonFiniteRun[c] > PN_PCM_NONFINITE_HOLD ? 0 : lastFinite[c] * (PN_PCM_NONFINITE_HOLD + 1 - nonFiniteRun[c]) / PN_PCM_NONFINITE_HOLD;
                } else {
                    h = v > 4.0f ? 4.0 : v < -4.0f ? -4.0 : v;         /* carried to +-4.0, saturated once at the store */
                    lastFinite[c] = h;
                    nonFiniteRun[c] = 0;
                }
                held[n * 2 + c] = h;
                continue;
            }
            int bits = (int)f.valid;
            double scale = ldexp(1.0, bits - 1);
            double q = x * scale + (bits < 32 && x != 0 ? r.Tpdf() : 0);
            double lo = -scale, hi = scale - 1;
            q = floor(q + 0.5);
            if (q > hi) q = hi;
            if (q < lo) q = lo;
            int64_t iv = (int64_t)q;
            held[n * 2 + c] = (double)iv / scale;
            uint32_t u;
            if (f.container == 16) { int16_t s = (int16_t)iv; memcpy(d, &s, 2); }
            else if (f.container == 24) { u = (uint32_t)(int32_t)iv; d[0] = (uint8_t)u; d[1] = (uint8_t)(u >> 8); d[2] = (uint8_t)(u >> 16); }
            else if (bits == 24) { int32_t s = (int32_t)((uint32_t)(int32_t)iv << 8); memcpy(d, &s, 4); }
            else { int32_t s = (int32_t)iv; memcpy(d, &s, 4); }
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Codec configurations                                                          */
/* ------------------------------------------------------------------------- */

enum PathKind { PATH_KSBC, PATH_HOST, PATH_FFSBC };

/* LMP feature masks for the baseband packet models (bthdef.h bit numbers). */
static const ULONGLONG kEdr3Dh5 = (1ull << 0) | (1ull << 1) | (1ull << 25) | (1ull << 26) | (1ull << 39) | (1ull << 40);

struct RadioModel { const char *name; BOOLEAN localValid; ULONGLONG local; BOOLEAN remoteValid; ULONGLONG remote; };
static const RadioModel kRadioUnknown = { "radio-unknown", FALSE, 0, FALSE, 0 };
static const RadioModel kRadio3Dh5 = { "3-DH5 both ends", TRUE, kEdr3Dh5, TRUE, kEdr3Dh5 };
static const RadioModel kRadioRemoteUnknown = { "3-DH5 local, sink features unknown", TRUE, kEdr3Dh5, FALSE, 0 };

struct ConfigDef {
    std::string key;
    std::string label;
    PathKind path;
    const PnCodecPlugin *api;
    A2DP_CODEC_CONFIG cfg;     /* A2dpSelectConfig output */
    ULONG mtu;
    ULONG engineRate;          /* engine (WaveRT) rate; 2x the codec rate engages the decimator */
    BOOLEAN xq;
    RadioModel radio;
    bool decodable;
    bool stress = false;       /* bitpool below the usable range: quality verdicts are informational */
    /* A state production reaches whose limit is documented and not fixed: its quality verdicts
     * are reported as that known issue (not as failures), and the state must not get worse
     * than recorded (the known* bounds fail the run). Never used to hide a fixable defect. */
    std::string known;
    double knownMaxThdn6 = 0, knownMinPinkSnr = 0, knownMaxClickHf = 0;
    std::string note;
};

static bool SelectFromIe(UCHAR mct, const std::vector<UCHAR> &ie, const PNA2DP_CONFIG &pref, ULONG rate, ULONG bits, A2DP_CODEC_CONFIG &cfg)
{
    A2DP_CODEC_CAPS caps;
    if (!A2dpParseCaps(mct, ie.data(), (ULONG)ie.size(), &caps, NULL, NULL)) return false;
    return A2dpSelectConfig(&caps, &pref, rate, bits, &cfg) != FALSE;
}

static PNA2DP_CONFIG DefaultPref()
{
    PNA2DP_CONFIG p = {};
    p.Size = sizeof(p); p.Version = PNA2DP_ABI_VERSION;
    p.Flags = PNA2DP_CFG_ABR_ENABLE | PNA2DP_CFG_SOFTWARE_VOLUME;
    p.TargetLatencyMs = 60; p.MaxLatencyMs = 250;
    p.AbrFloorBps = 128000;   /* regcfg.cpp default */
    p.Aac.Vbr = 1;
    p.Ldac.FormatMask = PNA2DP_FMT_S32; p.Ldac.Eqmid = PNA2DP_LDAC_EQMID_HQ; p.Ldac.AbrEnable = 1;
    return p;
}

static const std::vector<UCHAR> kSonySbcIe = { 0xFF, 0xFF, 0x02, 0x35 };

/* ------------------------------------------------------------------------- */
/* Stream emulation                                                              */
/* ------------------------------------------------------------------------- */

enum EventType { EV_VOLUME, EV_MUTE, EV_STALL, EV_STARVE, EV_ABR, EV_HOSTDROP, EV_RESTART, EV_PAUSE };
struct Event { double t; EventType type; double value; };

struct Job {
    std::string id;
    std::string group;
    int cfg;
    SigDef sig;
    int fmt = FMT_S16;
    double volDb = 0;
    std::vector<Event> events;
    bool noCrossfade = false;
    bool calibration = false;
    int repeat = 0;          /* determinism: encode twice, compare packets */
    int selftest = 0;        /* detector positive controls: 1 drop a packet, 2 duplicate one, 3 corrupt one, 4 add a -40 dBFS click */
};

struct Pkt {
    std::vector<uint8_t> bytes;
    ULONG pcmFrames;         /* PCM the write carries (last fragment of an AU) */
    ULONG frameCount;
    ULONG skipBefore;        /* RTP time of dropped blocks added before this packet */
    ULONG fragIndex, fragCount;
    ULONG expectBitpool;     /* SBC: bitpool the encoder used */
    ULONG hostPcmClaim;      /* host: pcm_frames_consumed */
    bool restart = false;    /* first packet of a restarted stream: the sink starts a new decoder */
};

struct Mark { size_t refFrame; std::string label; };

struct StreamResult {
    int channels = 0;
    ULONG codecRate = 0;
    std::vector<double> ref;           /* codec input (interleaved, normalised to +-1) */
    std::vector<double> ideal;         /* expected converter output (non-decimated, constant gain) */
    bool idealValid = false;
    std::vector<Pkt> pkts;
    std::vector<Mark> marks;
    ULONG framesPerPacket = 0, pcmPerCodecFrame = 0, sbcBitpool = 0, sbcFrameBytes = 0, sdu = 0;
    ULONG bitrate = 0, fitBitpool = 0, payloadRoom = 0;
    ULONG trims = 0, oversizeDrops = 0, hostLost = 0, hostInvalid = 0, encodeErrors = 0, convShort = 0, ringOverruns = 0;
    ULONG ldacExtraPcm = 0;
    long pauseUnsentFrames = -1;       /* EV_PAUSE: engine frames left unencoded once the pump drained */
    long pauseResidualFrames = -1;     /* EV_PAUSE: frames waiting for a whole encoder block at the pause */
    uint64_t engineFramesIn = 0;
    std::string error;
};

static ULONG TargetBytes(ULONG fmt) { return fmt == PNA2DP_FMT_S16 ? 2 : fmt == PNA2DP_FMT_S24 ? 3 : 4; }

static double ReadTarget(const uint8_t *p, ULONG fmt)
{
    if (fmt == PNA2DP_FMT_S16) { int16_t v; memcpy(&v, p, 2); return v / 32768.0; }
    if (fmt == PNA2DP_FMT_S24) { int32_t v = (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)); return (double)v / 2147483648.0; }
    if (fmt == PNA2DP_FMT_F32) { float v; memcpy(&v, p, 4); return v; }
    int32_t v; memcpy(&v, p, 4); return (double)v / 2147483648.0;
}

static bool CodecUsesRtp(ULONG c) { return !(c == PNA2DP_CODEC_APTX || c == PNA2DP_CODEC_APTX_LL || c == PNA2DP_CODEC_FASTSTREAM); }
static bool CodecUsesPh(ULONG c) { return c == PNA2DP_CODEC_SBC || c == PNA2DP_CODEC_LDAC; }

class Emu {
public:
    Emu(const ConfigDef &C, const Job &J, const PNA2DP_CONFIG &pref) : C(C), J(J), pref(pref) {}
    StreamResult Run();
    /* ABR ladder after stream start: {bitpool or EQMID or 0, bps} per level */
    std::vector<std::pair<ULONG, ULONG>> Ladder()
    {
        std::vector<std::pair<ULONG, ULONG>> out;
        if (!Start()) return out;
        for (ULONG l = 0; l <= abrMaxLevel; l++) {
            A2DP_CODEC_CONFIG b = cfg, o;
            b.LdacEqmid = (UCHAR)ldacBase;
            ULONG bps = A2dpQualityAtLevel(&b, l, pref.AbrFloorBps, &o);
            out.push_back({ cfg.Codec == PNA2DP_CODEC_SBC ? o.SbcBitpool : cfg.Codec == PNA2DP_CODEC_LDAC ? o.LdacEqmid : 0, bps });
        }
        if (enc) { C.api->close(enc); enc = nullptr; }
        return out;
    }

private:
    const ConfigDef &C;
    const Job &J;
    PNA2DP_CONFIG pref;
    StreamResult R;
    A2DP_CODEC_CONFIG cfg;
    bool host = false;
    PnCodecEncoder *enc = nullptr;
    PNA2DP_STREAM_PARAMS hp = {};
    ULONG mtu = 0, room = 0, txHdr = 0;
    bool useRtp = true, usePh = false;
    ULONG pcmPerCodecFrame = 0, fpp = 0, encFmt = PNA2DP_FMT_S32, decim = 1, targetCh = 2;
    sbc_encoder sbc;
    ULONG sbcCeiling = 0, ldacBase = 0, abrLevel = 0, abrMaxLevel = 0, sbcSdu = 0;
    sbc_params sbcStart = {};
    /* WaveRT fades (audio_wave.cpp WriteSegment) and restarts (PumpStopStream/PumpStartStream) */
    ULONG fadeFrames = 0, fadeInPos = 0, restartSkip = 0;
    bool fadeInActive = false, lastSegSilent = false, markRestart = false;
    double restartAt = -1;
    std::vector<uint8_t> tail;   /* audio_wave.cpp m_Tail: the newest fadeFrames frames held back */
    ULONG tailFrames = 0;
    /* ring (PumpConfigureInput / PumpWritePcm / RingRead / PumpTrim) */
    ULONG rate = 0, ch = 2, bits = 16, valid = 16; BOOLEAN isFloat = FALSE; ULONG bpf = 4;
    std::vector<uint8_t> ring; ULONG cap = 0; uint64_t rpos = 0, wpos = 0;
    PN_PCM_STATE ps;
    std::vector<uint8_t> fadeBuf;
    ULONG gain = 32768; bool muted = false;
    RTP_STATE rtp;
    PN_HOST_LEDGER ledger = {};
    ULONG seq = 1, pendingSkip = 0; bool dropNext = false;
    ULONG lastQ = 0, lastB = 0;
    std::vector<std::pair<uint64_t, std::string>> engineMarks;
    std::vector<uint8_t> pcmBuf, convBuf, outBuf;
    double gainExpected = 1.0;

    bool Start();
    void Send(const uint8_t *payload, ULONG bytes, ULONG frameCount, ULONG pcmFrames, ULONG phByte, ULONG skip, ULONG claim);
    void RingWrite(const uint8_t *d, ULONG len);
    ULONG RingAvail() const { return (ULONG)(wpos - rpos); }
    void RingRead(uint8_t *out, ULONG len);
    void Trim(ULONG keepMs);
    bool StepSbc();
    bool StepHost();
    void AppendRef(const uint8_t *conv, ULONG frames, ULONG fmt);
    void ResolveMarks(uint64_t engineStart, ULONG engineFrames, size_t refStart);
    void ApplyAbr(ULONG level);
    void Repack();
    void FadeTail(ULONG frames);
    bool FinishStream();
    void Restart();
    void Pause(bool race);
    void EmitPcm(const uint8_t *d, ULONG len);
    void FlushTail(bool fadeOut);
    ULONG LevelBitrate(ULONG l)
    {
        A2DP_CODEC_CONFIG b = cfg, o;
        b.LdacEqmid = (UCHAR)ldacBase;
        return A2dpQualityAtLevel(&b, l, pref.AbrFloorBps, &o);
    }
};

bool Emu::Start()
{
    cfg = C.cfg;
    mtu = PN_MIN(C.mtu, 2048u);   /* PN_PUMP_MAX_PACKET */
    useRtp = CodecUsesRtp(cfg.Codec);
    usePh = CodecUsesPh(cfg.Codec);
    txHdr = (useRtp ? RTP_HEADER_SIZE : 0) + (usePh ? 1 : 0);
    room = mtu - txHdr;
    R.payloadRoom = room;
    if (C.engineRate != cfg.SampleRate && C.engineRate != 2 * cfg.SampleRate) { R.error = "rate not supported"; return false; }
    decim = C.engineRate == cfg.SampleRate ? 1 : 2;
    host = C.path == PATH_HOST;
    ULONG sdu = PnBasebandSduBytes(C.radio.localValid, C.radio.local);
    switch (cfg.Codec) {
    case PNA2DP_CODEC_SBC: {
        sbc_params sp;
        sp.sample_rate = cfg.SampleRate;
        sp.channel_mode = (sbc_u8)(cfg.ChannelMode == PNA2DP_CHMODE_MONO ? SBC_MODE_MONO : cfg.ChannelMode == PNA2DP_CHMODE_DUAL ? SBC_MODE_DUAL :
                                   cfg.ChannelMode == PNA2DP_CHMODE_STEREO ? SBC_MODE_STEREO : SBC_MODE_JOINT);
        sp.allocation = cfg.SbcAlloc; sp.subbands = cfg.SbcSubbands; sp.blocks = cfg.SbcBlocks;
        /* PumpStartStream (pump.cpp), SBC branch: the production plan */
        ULONG xqSdu = PnSbcXqSduBytes(C.radio.localValid, C.radio.local, C.radio.remoteValid, C.radio.remote);
        A2DP_SBC_STREAM_PLAN plan;
        if (!A2dpSbcPlanStream(&cfg, C.xq, sdu, xqSdu, room, txHdr, pref.TargetLatencyMs, &plan)) { R.error = "SBC: no bitpool fits the MTU"; return false; }
        sp.bitpool = plan.StartBitpool;
        if (sbc_encoder_init(&sbc, &sp) != 0) { R.error = "sbc_encoder_init"; return false; }
        sbcStart = sp;
        cfg.SbcBitpool = cfg.SbcMaxBitpool = plan.StartBitpool;
        cfg.SbcFitBitpool = plan.FitBitpool;
        sbcCeiling = plan.CeilingBitpool;
        sdu = sbcSdu = plan.SduBytes;
        pcmPerCodecFrame = sbc.pcm_per_frame;
        encFmt = PNA2DP_FMT_S32;
        abrMaxLevel = 24;
        fpp = plan.FramesPerPacket;
        R.sbcBitpool = sp.bitpool; R.sbcFrameBytes = sbc.frame_length; R.fitBitpool = cfg.SbcFitBitpool;
        targetCh = sbc.channels;
        if (C.path == PATH_HOST) {
            /* SBC plugin through the ABI (PumpStartStream forces kernel SBC; the plugin is the
             * codec host's SBC test path) */
            encFmt = PNA2DP_FMT_S16;
        }
        break;
    }
    case PNA2DP_CODEC_AAC: {
        pcmPerCodecFrame = 1024; fpp = 1; encFmt = PNA2DP_FMT_S16; abrMaxLevel = 4;
        ULONG fit = PnAacMaxBitrateForPayload(room, cfg.SampleRate);
        if (fit) {
            if (!cfg.MaxBitrateBps || cfg.MaxBitrateBps > fit) cfg.MaxBitrateBps = fit;
            if (!cfg.BitrateBps || cfg.BitrateBps > fit) cfg.BitrateBps = fit;
        }
        break;
    }
    case PNA2DP_CODEC_APTX:
    case PNA2DP_CODEC_APTX_LL:
    case PNA2DP_CODEC_APTX_HD:
        pcmPerCodecFrame = 4; fpp = PnAptxBlocksPerPacket(cfg.Codec, room); encFmt = PNA2DP_FMT_S24; break;
    case PNA2DP_CODEC_LDAC:
        pcmPerCodecFrame = cfg.SampleRate >= 88200 ? 256 : 128; fpp = 2;
        encFmt = cfg.PcmFormat ? cfg.PcmFormat : PNA2DP_FMT_S32;
        ldacBase = PnLdacAbrQuality(cfg.LdacEqmid, 0);
        abrMaxLevel = 2 - ldacBase;
        break;
    default:
        R.error = "codec not emulated"; return false;
    }
    if (cfg.Codec != PNA2DP_CODEC_SBC) targetCh = cfg.Channels;
    R.sdu = sdu;
    /* ring: PumpConfigureInput */
    rate = C.engineRate; ch = 2;
    const EngineFmt &f = kFmts[J.fmt];
    bits = f.container; valid = f.valid; isFloat = f.isFloat ? TRUE : FALSE;
    bpf = ch * (bits / 8);
    cap = (ULONG)((ULONGLONG)rate * 600 / 1000) * bpf;   /* PN_PUMP_RING_MS */
    cap = ((cap + 4095) / 4096) * 4096;
    cap -= cap % bpf;
    ring.assign(cap, 0);
    while (fpp > 1 && fpp * pcmPerCodecFrame * decim * bpf > 32768) fpp--;   /* PN_PUMP_MAX_PCM_BLOCK */
    fadeFrames = PnWaveFadeFrames(rate, rate / 100, 4096 / bpf);              /* PN_WAVE_FADE_SCRATCH */
    cfg.BitrateBps = A2dpNominalBitrate(&cfg);
    R.bitrate = cfg.BitrateBps;
    /* PumpStartStream: rungs with the same bitrate as the one above are not counted */
    while (abrMaxLevel && LevelBitrate(abrMaxLevel) == LevelBitrate(abrMaxLevel - 1)) abrMaxLevel--;
    R.framesPerPacket = fpp;
    R.pcmPerCodecFrame = pcmPerCodecFrame;
    PnPcmStateReset(&ps, decim, FALSE);   /* the stream starts at the programme's own start */
    ps.Dither = host && encFmt == PNA2DP_FMT_S16;
    fadeBuf.assign(J.noCrossfade ? 0 : 8192, 0);   /* PN_PUMP_FADE_BYTES */
    RtpInit(&rtp, 96);
    gain = PnVolumeGainQ15((LONG)(J.volDb * 65536.0));
    gainExpected = gain / 32768.0;
    R.codecRate = cfg.SampleRate;
    R.channels = (int)targetCh;
    pcmBuf.assign(32768, 0); convBuf.assign(65536, 0); outBuf.assign(8192 + 64, 0);
    if (host) {
        hp.Codec = cfg.Codec; hp.SampleRate = cfg.SampleRate; hp.Channels = cfg.Channels; hp.PcmFormat = encFmt;
        hp.ChannelMode = cfg.ChannelMode; hp.BitrateBps = cfg.BitrateBps; hp.MediaMtu = room;
        hp.QualityIndex = cfg.Codec == PNA2DP_CODEC_LDAC ? cfg.LdacEqmid : 0;
        hp.CodecInfoLength = cfg.IesLength;
        memcpy(hp.CodecInfo, cfg.Ies, cfg.IesLength);
        if (!C.api) { R.error = "plugin missing"; return false; }
        enc = C.api->open(&hp);
        if (!enc) { R.error = "plugin open failed"; return false; }
        uint32_t mo = C.api->max_output_bytes ? C.api->max_output_bytes(&hp) : 0;
        outBuf.assign((mo < 4096 ? 4096 : mo) + 64, 0);
        lastQ = hp.QualityIndex; lastB = hp.BitrateBps;
    }
    return true;
}

void Emu::RingWrite(const uint8_t *d, ULONG len)
{
    if (len >= cap) { d += len - cap + bpf; len = cap - bpf; }
    ULONG avail = RingAvail();
    if (avail + len > cap) {
        ULONG drop = avail + len - cap;
        drop = ((drop + bpf - 1) / bpf) * bpf;
        rpos += drop;
        R.ringOverruns++;
    }
    ULONG off = (ULONG)(wpos % cap), first = PN_MIN(len, cap - off);
    memcpy(&ring[off], d, first);
    if (len > first) memcpy(&ring[0], d + first, len - first);
    wpos += len;
}

void Emu::RingRead(uint8_t *out, ULONG len)
{
    ULONG off = (ULONG)(rpos % cap), first = PN_MIN(len, cap - off);
    memcpy(out, &ring[off], first);
    if (len > first) memcpy(out + first, &ring[0], len - first);
    rpos += len;
}

/* PumpTrim (pump.cpp) with the production PnTrimPlan. */
void Emu::Trim(ULONG keepMs)
{
    ULONG fade = 0;
    ULONG keepBytes = (ULONG)((ULONGLONG)rate * keepMs / 1000) * bpf;
    ULONG drop = PnTrimPlan(RingAvail(), keepBytes, bpf, rate * PN_TRIM_FADE_MS / 1000, fadeBuf.empty() ? 0 : (ULONG)fadeBuf.size(), &fade);
    if (fade && !fadeBuf.empty()) {
        ULONG off = (ULONG)(rpos % cap), len = fade * bpf, first = PN_MIN(len, cap - off);
        memcpy(fadeBuf.data(), &ring[off], first);
        if (len > first) memcpy(fadeBuf.data() + first, &ring[0], len - first);
    }
    rpos += drop;
    if (!drop) return;
    ps.FadeOut = fade ? fadeBuf.data() : NULL;
    ps.FadeFrames = fade;
    ps.FadePos = 0;
    R.trims++;
    R.marks.push_back({ R.ref.size() / targetCh, Sfmt("trim %u ms (crossfade %u frames)", drop / bpf * 1000 / rate, fade) });
}

void Emu::AppendRef(const uint8_t *conv, ULONG frames, ULONG fmt)
{
    ULONG b = TargetBytes(fmt);
    for (ULONG i = 0; i < frames * targetCh; i++) R.ref.push_back(ReadTarget(conv + i * b, fmt));
}

void Emu::ResolveMarks(uint64_t engineStart, ULONG engineFrames, size_t refStart)
{
    for (auto it = engineMarks.begin(); it != engineMarks.end();) {
        if (it->first < engineStart + engineFrames) {
            uint64_t off = it->first > engineStart ? it->first - engineStart : 0;
            R.marks.push_back({ refStart + (size_t)(off / decim), it->second });
            it = engineMarks.erase(it);
        } else ++it;
    }
}

void Emu::ApplyAbr(ULONG level)
{
    if (level > abrMaxLevel) level = abrMaxLevel;
    abrLevel = level;
    A2DP_CODEC_CONFIG base = cfg, out;
    base.LdacEqmid = (UCHAR)ldacBase;
    A2dpQualityAtLevel(&base, level, pref.AbrFloorBps, &out);   /* AbrApplyLevel */
    cfg = out;
    if (cfg.Codec == PNA2DP_CODEC_SBC && !host) sbc_encoder_set_bitpool(&sbc, cfg.SbcBitpool);
    if (cfg.Codec == PNA2DP_CODEC_SBC) Repack();
    R.marks.push_back({ R.ref.size() / targetCh, cfg.Codec == PNA2DP_CODEC_SBC ? Sfmt("ABR level %u: bitpool %u", level, cfg.SbcBitpool) :
                        cfg.Codec == PNA2DP_CODEC_LDAC ? Sfmt("ABR level %u: EQMID %u", level, cfg.LdacEqmid) :
                        Sfmt("ABR level %u: %u bps", level, cfg.BitrateBps) });
}

/* PumpSbcRepack (pump.cpp): frames per packet for the current bitpool. */
void Emu::Repack()
{
    ULONG n = A2dpSbcFramesPerPacket(&cfg, pref.TargetLatencyMs, room, txHdr, sbcSdu);
    if (!n) n = 1;
    while (n > 1 && n * pcmPerCodecFrame * decim * bpf > 32768) n--;
    fpp = n;
}

/* RingFadeTailLocked (pump.cpp): the newest frames fade out in place. */
void Emu::FadeTail(ULONG frames)
{
    uint64_t start = wpos - (uint64_t)frames * bpf;
    ULONG done = 0;
    while (done < frames) {
        ULONG off = (ULONG)((start + (uint64_t)done * bpf) % cap), n = PN_MIN(frames - done, (cap - off) / bpf);
        if (!n) break;
        PnPcmRamp(&ring[off], n, ch, bits, valid, isFloat, done, frames, FALSE);
        done += n;
    }
}

/* CPnMiniportWaveRTStream::EmitPcm (audio_wave.cpp): through the hold-back FIFO. */
void Emu::EmitPcm(const uint8_t *d, ULONG len)
{
    ULONG keep = fadeFrames * bpf, t = tailFrames * bpf;
    if (!keep) { RingWrite(d, len); return; }
    if (tail.size() < keep) tail.assign(keep, 0);
    if (t + len <= keep) { memcpy(&tail[t], d, len); tailFrames += len / bpf; return; }
    ULONG out = t + len - keep, fromTail = PN_MIN(out, t), fromData = out - fromTail;
    if (fromTail) RingWrite(tail.data(), fromTail);
    if (fromData) RingWrite(d, fromData);
    ULONG left = t - fromTail;
    if (left) memmove(tail.data(), tail.data() + fromTail, left);
    memcpy(tail.data() + left, d + fromData, len - fromData);
    tailFrames = keep / bpf;
}

/* CPnMiniportWaveRTStream::FlushTail: the fade spans the held-back frames */
void Emu::FlushTail(bool fadeOut)
{
    if (!tailFrames) return;
    if (fadeOut && tailFrames <= fadeFrames) PnPcmRamp(tail.data(), tailFrames, ch, bits, valid, isFloat, 0, tailFrames, FALSE);
    RingWrite(tail.data(), tailFrames * bpf);
    tailFrames = 0;
}

/* PumpFinishStream (pump.cpp): KS pause or the engine's end of stream - the held-back frames
 * join the ring, the waiting PCM fades, is padded (also an empty ring) and drains, all in one
 * ring-lock hold (findings #1/#7). */
bool Emu::FinishStream()
{
    if (tailFrames) { RingWrite(tail.data(), tailFrames * bpf); tailFrames = 0; }
    ULONG block = fpp * pcmPerCodecFrame * decim * bpf, avail = RingAvail();
    if (!block) return false;
    if (avail) FadeTail(PN_MIN(avail / bpf, rate * PN_TRIM_FADE_MS / 1000));
    ULONG residual = avail % block;
    ULONG pad = (residual ? block - residual : 0) + (cfg.Codec == PNA2DP_CODEC_AAC ? 2 : 1) * block;
    ULONG roomBytes = cap - avail;
    if (pad > roomBytes) pad = roomBytes - roomBytes % block;
    std::vector<uint8_t> z(pad, 0);
    RingWrite(z.data(), pad);
    return pad != 0;
}

/* A KS pause and resume of the same stream: the pump finishes the block (above) and sends
 * what it can; the stream then resumes where it stopped, fading in. Race (findings #1/#7):
 * the pump thread takes every whole block it can right up to the hand-over (before the fix,
 * the hand-over of the held-back frames woke it and it encoded them unfaded before the fade),
 * and the engine stays paused for 50 ms, which the receiver plays as silence (an underrun;
 * modelled as 50 ms of encoded digital silence, so every codec, RTP or not, keeps its
 * timeline). */
void Emu::Pause(bool race)
{
    if (race) for (int guard = 0; guard < 256 && (host ? StepHost() : StepSbc()); guard++) {}
    R.pauseResidualFrames = (long)((RingAvail() / bpf + tailFrames) % PN_MAX(fpp * pcmPerCodecFrame * decim, 1u));
    size_t before = R.ref.size() / targetCh + (RingAvail() / bpf + tailFrames) / decim;
    bool padded = FinishStream();   /* SetState(PAUSE) */
    for (int guard = 0; guard < 256 && (host ? StepHost() : StepSbc()); guard++) {}
    R.pauseUnsentFrames = (long)(RingAvail() / bpf);
    if (race) {
        std::vector<uint8_t> z((size_t)(rate / 20) * bpf, 0);
        RingWrite(z.data(), (ULONG)z.size());
        for (int guard = 0; guard < 256 && (host ? StepHost() : StepSbc()); guard++) {}
    }
    if (padded) {
        R.marks.push_back({ before, "pause: fade-out, padded" });
        R.marks.push_back({ R.ref.size() / targetCh + RingAvail() / bpf / decim, "resume: fade-in" });
    }
    /* SetState(RUN): the resumed audio fades in only from the silence the pump padded */
    fadeInActive = padded && fadeFrames != 0;
    fadeInPos = 0;
}

/* PumpStopStream + PumpStartStream after an outage (recovery, reconfigure): the ring is
 * flushed, the encoder starts over and, audio having been discarded, the first block fades in. */
void Emu::Restart()
{
    ULONG avail = RingAvail();
    ULONG flushed = avail / bpf / decim;
    /* PumpStartStream: the first block fades in when the discarded PCM ended in a signal (#4) */
    BOOLEAN midProgramme = PnRingTailHasSignal(ring.data(), cap, wpos, avail, rate * PN_TRIM_FADE_MS / 1000 * bpf);
    rpos = wpos;
    PnPcmStateReset(&ps, decim, midProgramme);
    ps.Dither = host && encFmt == PNA2DP_FMT_S16;
    if (cfg.Codec == PNA2DP_CODEC_SBC && !host) { sbc_params sp = sbcStart; sp.bitpool = sbc.p.bitpool; sbc_encoder_init(&sbc, &sp); }
    if (host && enc) {
        C.api->close(enc);
        enc = C.api->open(&hp);
        RtlZeroMemory(&ledger, sizeof(ledger));
        lastQ = hp.QualityIndex; lastB = hp.BitrateBps;
    }
    /* the receiver hears nothing for the flushed audio: the RTP timeline skips it */
    if (host) pendingSkip += flushed; else restartSkip += flushed;
    R.ref.insert(R.ref.end(), (size_t)flushed * targetCh, 0.0);
    R.marks.push_back({ R.ref.size() / targetCh, "stream restart: first block after the outage" });
    markRestart = true;
}

/* SendMediaPacket (pump.cpp), replicated minimally: header, fragmentation, timestamps. */
void Emu::Send(const uint8_t *payload, ULONG bytes, ULONG frameCount, ULONG pcmFrames, ULONG phByte, ULONG skip, ULONG claim)
{
    ULONG ts = rtp.Timestamp;
    ULONG count = (bytes + room - 1) / room;
    if ((count > 1 && cfg.Codec != PNA2DP_CODEC_AAC) || count > 8 /* PN_L2CAP_MAX_WRITES */ || count > 4 /* MaxWritesInFlight */) {
        rtp.Timestamp += pcmFrames;
        R.oversizeDrops++;
        return;
    }
    ULONG offset = 0;
    for (ULONG i = 0; i < count; i++) {
        Pkt p;
        ULONG n = PN_MIN(room, bytes - offset);
        bool last = i + 1 == count;
        p.bytes.assign(txHdr + n, 0);
        if (useRtp) RtpWriteHeader(&rtp, p.bytes.data(), last ? pcmFrames : 0, cfg.Codec == PNA2DP_CODEC_AAC && last);
        if (usePh) p.bytes[txHdr - 1] = (UCHAR)(phByte ? phByte : (frameCount & 0x0f));
        memcpy(p.bytes.data() + txHdr, payload + offset, n);
        p.pcmFrames = last ? pcmFrames : 0;
        p.frameCount = frameCount;
        p.skipBefore = i == 0 ? skip : 0;
        p.fragIndex = i; p.fragCount = count;
        p.expectBitpool = cfg.Codec == PNA2DP_CODEC_SBC ? (host ? cfg.SbcBitpool : sbc.p.bitpool) : 0;
        p.hostPcmClaim = claim;
        if (i == 0 && markRestart) { p.restart = true; markRestart = false; }
        R.pkts.push_back(std::move(p));
        offset += n;
    }
    rtp.Timestamp = ts + pcmFrames;
}

bool Emu::StepSbc()
{
    ULONG pcmFrames = fpp * pcmPerCodecFrame, ringFrames = pcmFrames * decim, need = ringFrames * bpf;
    if (need > 32768 || RingAvail() < need) return false;
    uint64_t engineStart = rpos / bpf;
    RingRead(pcmBuf.data(), need);
    size_t refStart = R.ref.size() / targetCh;
    ULONG conv = PnConvertPcmEx(&ps, pcmBuf.data(), need, ch, bits, valid, isFloat, convBuf.data(), 32768,
                                PNA2DP_FMT_S32, sbc.channels, muted ? 0 : gain);
    if (conv < pcmFrames * sbc.channels * 4) { R.convShort++; return false; }
    AppendRef(convBuf.data(), pcmFrames, PNA2DP_FMT_S32);
    ResolveMarks(engineStart, ringFrames, refStart);
    const sbc_s32 *s32 = (const sbc_s32 *)convBuf.data();
    ULONG n = 0, count = 0;
    for (ULONG f = 0; f < fpp; f++) {
        ULONG len = sbc_encode_frame_s32(&sbc, s32 + f * pcmPerCodecFrame * sbc.channels, outBuf.data() + n, (ULONG)outBuf.size() - n);
        if (!len) break;
        n += len; count++;
    }
    if (!count) { R.encodeErrors++; return true; }
    ULONG skip = restartSkip;
    restartSkip = 0;
    rtp.Timestamp += skip;
    Send(outBuf.data(), n, count, count * pcmPerCodecFrame, count & 0x0F, skip, 0);
    return true;
}

bool Emu::StepHost()
{
    ULONG pcmFrames = fpp * pcmPerCodecFrame, ringFrames = pcmFrames * decim, need = ringFrames * bpf;
    if (need > 32768 || RingAvail() < need) return false;
    uint64_t engineStart = rpos / bpf;
    RingRead(pcmBuf.data(), need);
    size_t refStart = R.ref.size() / targetCh;
    ULONG conv = PnConvertPcmEx(&ps, pcmBuf.data(), need, ch, bits, valid, isFloat, convBuf.data(), 65536,
                                encFmt, targetCh, muted ? 0 : gain);
    AppendRef(convBuf.data(), conv / (targetCh * TargetBytes(encFmt)), encFmt);
    ResolveMarks(engineStart, ringFrames, refStart);
    ULONG sequence = seq++;
    if (!conv || !PnHostLedgerAdd(&ledger, sequence, pcmFrames, 0)) { R.convShort++; return true; }
    if (dropNext) {
        /* the request is lost at the host: the next result writes it off */
        dropNext = false;
        R.hostLost++;
        R.marks.push_back({ refStart, Sfmt("host block lost (%u frames)", pcmFrames) });
        return true;
    }
    /* device.cpp Encode: quality changes become reconfigure() */
    ULONG q = cfg.Codec == PNA2DP_CODEC_LDAC ? cfg.LdacEqmid : cfg.Codec == PNA2DP_CODEC_SBC ? cfg.SbcBitpool : abrLevel;
    ULONG b = cfg.BitrateBps;
    if (q != lastQ || b != lastB) {
        if (C.api->reconfigure && C.api->reconfigure(enc, q, b) == 0) { lastQ = q; lastB = b; }
    }
    PnCodecOutput o = {};
    o.buffer = outBuf.data(); o.capacity = (uint32_t)outBuf.size() - 64;
    int rc = C.api->encode(enc, convBuf.data(), conv, pcmFrames, &o);
    if (rc) R.encodeErrors++;
    bool ok = rc == 0 && PnHostResultValid(o.written, 2048 /* PN_PUMP_MAX_PACKET */, o.frame_count, o.payload_header_byte, usePh);
    PN_HOST_MATCH m = PnHostLedgerMatch(&ledger, sequence);
    if (!m.Matched) { R.hostInvalid++; return true; }
    pendingSkip += m.LostPcmFrames;
    if (!ok) {
        R.hostInvalid++;
        pendingSkip += PnHostLedgerReturn(&ledger, m.SubmittedPcmFrames);
        return true;
    }
    ULONG pcm = PnHostLedgerReturn(&ledger, o.pcm_frames_consumed);
    if (cfg.Codec == PNA2DP_CODEC_LDAC && o.frame_count && o.pcm_frames_consumed > o.frame_count * pcmPerCodecFrame)
        R.ldacExtraPcm += o.pcm_frames_consumed - o.frame_count * pcmPerCodecFrame;
    ULONG skip = pendingSkip;
    pendingSkip = 0;
    /* PumpStepHostDrain */
    rtp.Timestamp += skip;
    if (o.written) Send(o.buffer, o.written, o.frame_count, pcm, o.payload_header_byte, skip, o.pcm_frames_consumed);
    else rtp.Timestamp += pcm;
    return true;
}

StreamResult Emu::Run()
{
    if (!Start()) { if (enc) C.api->close(enc); return R; }
    /* engine signal */
    double fs = (double)C.engineRate;
    size_t frames = (size_t)((J.sig.dur + 0.3) * fs);
    std::vector<double> L, Rr;
    GenSignal(J.sig, fs, frames, L, Rr);
    std::vector<uint8_t> bytes;
    std::vector<double> held;
    EncodeEngine(L, Rr, kFmts[J.fmt], bytes, held);
    R.engineFramesIn = frames;
    /* expected converter output for the conversion check: constant gain, no decimation */
    if (decim == 1 && J.events.empty()) {
        R.idealValid = true;
        R.ideal.reserve(frames * targetCh);
        for (size_t n = 0; n < frames; n++) {
            auto sat = [](double v) { return v > 2147483647.0 / 2147483648.0 ? 2147483647.0 / 2147483648.0 : v < -1.0 ? -1.0 : v; };
            if (targetCh == 1) R.ideal.push_back(sat((held[n * 2] + held[n * 2 + 1]) / 2 * gainExpected));
            else { R.ideal.push_back(sat(held[n * 2] * gainExpected)); R.ideal.push_back(sat(held[n * 2 + 1] * gainExpected)); }
        }
    }
    std::vector<Event> ev = J.events;
    std::sort(ev.begin(), ev.end(), [](const Event &a, const Event &b) { return a.t < b.t; });
    size_t evi = 0;
    ULONGLONG rem = 0;
    size_t pos = 0;
    const ULONG packetFrames = rate / 100;   /* 10 ms WaveRT packet */
    std::set<uint64_t> starvedPackets;
    double stallUntil = -1;
    ULONG effTarget, effMax;
    PnEffectiveLatencyMs(pref.TargetLatencyMs, pref.MaxLatencyMs, 600, &effTarget, &effMax);
    std::vector<uint8_t> zero(bpf * 2048, 0), fadeTmp(bpf * 2048, 0);
    double now = 0;
    while (pos < frames) {
        ULONG due = (ULONG)PnWaveFramesDue(20000, rate, &rem);   /* 2 ms render tick */
        now = (double)pos / fs;
        while (evi < ev.size() && ev[evi].t <= now) {
            const Event &e = ev[evi++];
            switch (e.type) {
            case EV_VOLUME: gain = PnVolumeGainQ15((LONG)(e.value * 65536.0)); engineMarks.push_back({ pos, Sfmt("volume %.1f dB", e.value) }); break;
            case EV_MUTE: muted = e.value != 0; engineMarks.push_back({ pos, e.value ? "mute" : "unmute" }); break;
            case EV_STALL: stallUntil = now + e.value / 1000.0; break;
            case EV_STARVE: {
                uint64_t pkt = (pos + packetFrames - 1) / packetFrames;
                starvedPackets.insert(pkt);
                engineMarks.push_back({ pkt * packetFrames, "starved 10 ms packet -> silence" });
                engineMarks.push_back({ (pkt + 1) * packetFrames, "starvation end" });
                break;
            }
            case EV_ABR: ApplyAbr((ULONG)e.value); break;
            case EV_HOSTDROP: dropNext = true; break;
            case EV_RESTART:
                R.marks.push_back({ R.ref.size() / targetCh, "outage: audio lost (stream restarts)" });
                stallUntil = restartAt = now + e.value / 1000.0;
                break;
            case EV_PAUSE: Pause(e.value != 0); break;
            }
        }
        size_t n = PN_MIN((size_t)due, frames - pos);
        /* WaveRT tick (audio_wave.cpp WriteSegment): starved packets are played as silence
         * (PnWavePacketSilent); the audio before the gap fades out in the ring and the audio
         * after it fades in */
        size_t k = 0;
        while (k < n) {
            uint64_t pkt = (pos + k) / packetFrames;
            size_t run = PN_MIN(n - k, (size_t)((pkt + 1) * packetFrames - (pos + k)));
            run = PN_MIN(run, (size_t)2048);
            bool silent = starvedPackets.count(pkt) != 0;
            if (silent) {
                FlushTail(!lastSegSilent);
                RingWrite(zero.data(), (ULONG)(run * bpf));
                fadeInActive = fadeFrames != 0;
                fadeInPos = 0;
            } else if (fadeInActive && fadeInPos < fadeFrames) {
                ULONG nf = PN_MIN((ULONG)run, fadeFrames - fadeInPos);
                memcpy(fadeTmp.data(), &bytes[(pos + k) * bpf], (size_t)nf * bpf);
                PnPcmRamp(fadeTmp.data(), nf, ch, bits, valid, isFloat, fadeInPos, fadeFrames, TRUE);
                EmitPcm(fadeTmp.data(), nf * bpf);
                fadeInPos += nf;
                if (fadeInPos >= fadeFrames) fadeInActive = false;
                if (run > nf) EmitPcm(&bytes[(pos + k + nf) * bpf], (ULONG)((run - nf) * bpf));
            } else EmitPcm(&bytes[(pos + k) * bpf], (ULONG)(run * bpf));
            lastSegSilent = silent;
            k += run;
        }
        pos += n;
        /* pump thread: latency guard, then steps unless the link is stalled */
        ULONG latencyUs = (ULONG)((ULONGLONG)(RingAvail() / bpf) * 1000000ull / rate);
        if (latencyUs > effMax * 1000) Trim(effTarget);
        if (restartAt >= 0 && now >= restartAt) { restartAt = -1; Restart(); }
        if (now >= stallUntil) {
            int guard = 0;
            while (guard++ < 256 && (host ? StepHost() : StepSbc())) {}
        }
    }
    /* end of stream: the held-back frames follow and the sound ends as at a pause (Tick at EOS) */
    FinishStream();
    for (int guard = 0; now >= stallUntil && guard < 256 && (host ? StepHost() : StepSbc()); guard++) {}
    if (enc) C.api->close(enc);
    return R;
}

/* ------------------------------------------------------------------------- */
/* Receiver: packet checks and decoder streams                                  */
/* ------------------------------------------------------------------------- */

struct RxResult {
    std::vector<uint8_t> stream;      /* decoder input */
    std::vector<uint32_t> decFrames;  /* PCM frames each decodable unit yields, in order */
    std::vector<uint64_t> tlPos;      /* timeline position of each unit */
    uint64_t tlFrames = 0;
    ULONG errors = 0;
    std::string firstError;
    ULONG packets = 0, maxPacket = 0, minPacket = 0xffffffff, fragmented = 0, units = 0, codecFrames = 0;
    double avgPacket = 0, framesPerPacketAvg = 0;
    uint64_t payloadBytes = 0, l2capBytes = 0;
    uint64_t slots = 0;
    ULONG bitpools[256] = {};
    ULONG ldacFrameBytesMin = 0xffffffff, ldacFrameBytesMax = 0;
    uint64_t skippedFrames = 0;
    size_t splitAt = 0;               /* decoder input offset where a restarted stream begins (0 = none) */
};

static void RxError(RxResult &rx, const std::string &m) { if (!rx.errors++) rx.firstError = m; }

static RxResult Receive(const ConfigDef &C, const StreamResult &S)
{
    RxResult rx;
    ULONG codec = C.cfg.Codec;
    bool useRtp = CodecUsesRtp(codec), usePh = CodecUsesPh(codec);
    ULONG hdr = (useRtp ? RTP_HEADER_SIZE : 0) + (usePh ? 1 : 0);
    bool haveTs = false;
    USHORT lastSeq = 0; ULONG lastTs = 0, ts0 = 0, expectAdvance = 0;
    uint64_t cursor = 0;
    std::vector<uint8_t> au;
    ULONG auTs = 0;
    double fpSum = 0;
    for (size_t i = 0; i < S.pkts.size(); i++) {
        const Pkt &p = S.pkts[i];
        const uint8_t *b = p.bytes.data();
        ULONG len = (ULONG)p.bytes.size();
        if (p.restart && !rx.splitAt) rx.splitAt = rx.stream.size();
        rx.packets++;
        rx.maxPacket = PN_MAX(rx.maxPacket, len); rx.minPacket = PN_MIN(rx.minPacket, len);
        rx.l2capBytes += len;
        if (len > C.mtu) RxError(rx, Sfmt("packet %zu: %u bytes > MTU %u", i, len, C.mtu));
        if (S.sdu) rx.slots += PnSduSlots(S.sdu, len);
        USHORT seqv = 0; ULONG ts = 0, hl = 0;
        if (useRtp) {
            if (!RtpParseHeader(b, len, &seqv, &ts, &hl) || hl != RTP_HEADER_SIZE) { RxError(rx, Sfmt("packet %zu: bad RTP header", i)); continue; }
            if ((b[1] & 0x7f) != 96) RxError(rx, Sfmt("packet %zu: payload type %u", i, b[1] & 0x7f));
            bool marker = (b[1] & 0x80) != 0;
            bool expectMarker = codec == PNA2DP_CODEC_AAC && p.fragIndex + 1 == p.fragCount;
            if (marker != expectMarker) RxError(rx, Sfmt("packet %zu: marker %d", i, marker));
            if (haveTs) {
                if ((USHORT)(lastSeq + 1) != seqv) RxError(rx, Sfmt("packet %zu: sequence %u after %u", i, seqv, lastSeq));
                ULONG adv = ts - lastTs;
                ULONG want = p.fragIndex ? 0 : expectAdvance + p.skipBefore;
                if (adv != want) RxError(rx, Sfmt("packet %zu: timestamp advanced %u, expected %u", i, adv, want));
            } else ts0 = ts;
            lastSeq = seqv; lastTs = ts; haveTs = true;
            if (p.fragIndex + 1 == p.fragCount) expectAdvance = p.pcmFrames; else expectAdvance = 0;
            rx.skippedFrames += p.skipBefore;
        }
        const uint8_t *pl = b + hdr;
        ULONG plen = len - hdr;
        rx.payloadBytes += plen;
        if (p.fragCount > 1) rx.fragmented++;
        uint64_t pos = useRtp ? (uint64_t)(ULONG)(ts - ts0) : cursor;
        switch (codec) {
        case PNA2DP_CODEC_SBC: {
            ULONG count = b[hdr - 1] & 0x0f;
            if (b[hdr - 1] & 0xe0) RxError(rx, Sfmt("packet %zu: SBC fragmentation bits set", i));
            ULONG off = 0, n = 0, pcm = 0;
            while (off < plen) {
                sbc_params sp;
                ULONG fl = sbc_parse_header(pl + off, plen - off, &sp);
                if (!fl || off + fl > plen) { RxError(rx, Sfmt("packet %zu: bad SBC frame at %u", i, off)); break; }
                if (sp.sample_rate != C.cfg.SampleRate || sp.subbands != C.cfg.SbcSubbands || sp.blocks != C.cfg.SbcBlocks)
                    RxError(rx, Sfmt("packet %zu: SBC frame parameters differ from the configuration", i));
                if (p.expectBitpool && sp.bitpool != p.expectBitpool) RxError(rx, Sfmt("packet %zu: bitpool %u, encoder at %u", i, sp.bitpool, p.expectBitpool));
                rx.bitpools[sp.bitpool]++;
                pcm += sbc_pcm_per_frame(&sp);
                off += fl; n++;
            }
            if (n != count) RxError(rx, Sfmt("packet %zu: header says %u frames, %u present", i, count, n));
            if (pcm != p.pcmFrames) RxError(rx, Sfmt("packet %zu: carries %u PCM frames, timestamp advances %u", i, pcm, p.pcmFrames));
            rx.stream.insert(rx.stream.end(), pl, pl + plen);
            rx.decFrames.push_back(pcm); rx.tlPos.push_back(pos); rx.units++;
            rx.codecFrames += n; fpSum += n;
            cursor = pos + pcm;
            break;
        }
        case PNA2DP_CODEC_AAC: {
            if (p.fragIndex == 0) { au.clear(); auTs = ts; }
            else if (ts != auTs) RxError(rx, Sfmt("packet %zu: AAC fragment timestamp differs", i));
            au.insert(au.end(), pl, pl + plen);
            if (p.fragIndex + 1 == p.fragCount) {
                if (au.size() >= 8192) { RxError(rx, "AAC access unit too large for LOAS"); break; }
                uint8_t loas[3] = { 0x56, (uint8_t)(0xe0 | (au.size() >> 8)), (uint8_t)au.size() };
                rx.stream.insert(rx.stream.end(), loas, loas + 3);
                rx.stream.insert(rx.stream.end(), au.begin(), au.end());
                rx.decFrames.push_back(1024); rx.tlPos.push_back((uint64_t)(ULONG)(auTs - ts0)); rx.units++;
                rx.codecFrames++; fpSum += 1;
                if (p.pcmFrames != 1024) RxError(rx, Sfmt("packet %zu: AAC access unit carries %u PCM frames", i, p.pcmFrames));
                cursor = (uint64_t)(ULONG)(auTs - ts0) + 1024;
            }
            break;
        }
        case PNA2DP_CODEC_APTX: case PNA2DP_CODEC_APTX_LL: case PNA2DP_CODEC_APTX_HD: {
            /* libfreeaptx: 4 PCM frames -> one 4-byte (aptX) or 6-byte (aptX HD) codeword pair */
            ULONG blk = codec == PNA2DP_CODEC_APTX_HD ? 6 : 4;
            if (plen % blk) RxError(rx, Sfmt("packet %zu: %u bytes is not a whole number of aptX blocks", i, plen));
            ULONG pcm = plen / blk * 4;
            if (pcm != p.pcmFrames) RxError(rx, Sfmt("packet %zu: carries %u PCM frames, accounted %u", i, pcm, p.pcmFrames));
            rx.stream.insert(rx.stream.end(), pl, pl + plen);
            rx.decFrames.push_back(pcm); rx.tlPos.push_back(pos); rx.units++;
            rx.codecFrames += plen / blk; fpSum += plen / blk;
            cursor = pos + pcm;
            break;
        }
        case PNA2DP_CODEC_LDAC: {
            ULONG count = b[hdr - 1] & 0x0f;
            /* LDAC frame header (pack_ldac.c): sync 0xAA, rate(3) chconfig(2) frame_length-1(9) status(2) */
            ULONG off = 0, n = 0;
            while (off + 3 <= plen) {
                if (pl[off] != 0xAA) { RxError(rx, Sfmt("packet %zu: LDAC sync missing at %u", i, off)); break; }
                ULONG fl = ((((ULONG)pl[off + 1] & 0x07) << 6) | (pl[off + 2] >> 2)) + 1;   /* frame_length-1 (9 bits) */
                ULONG total = (off + fl + 3 == plen || (off + fl + 3 < plen && pl[off + fl + 3] == 0xAA)) ? fl + 3 : fl;
                if (off + total > plen) { RxError(rx, Sfmt("packet %zu: LDAC frame overruns the packet", i)); break; }
                rx.ldacFrameBytesMin = PN_MIN(rx.ldacFrameBytesMin, total); rx.ldacFrameBytesMax = PN_MAX(rx.ldacFrameBytesMax, total);
                off += total; n++;
            }
            if (off != plen) RxError(rx, Sfmt("packet %zu: LDAC frames end at %u of %u", i, off, plen));
            if (n != count || !n) RxError(rx, Sfmt("packet %zu: header says %u LDAC frames, %u present", i, count, n));
            ULONG per = C.cfg.SampleRate >= 88200 ? 256 : 128;
            if (p.pcmFrames != n * per) RxError(rx, Sfmt("packet %zu: %u LDAC frames but timestamp advances %u", i, n, p.pcmFrames));
            rx.codecFrames += n; fpSum += n; rx.units++;
            cursor = pos + p.pcmFrames;
            break;
        }
        }
    }
    rx.tlFrames = cursor;
    rx.avgPacket = rx.packets ? (double)rx.l2capBytes / rx.packets : 0;
    rx.framesPerPacketAvg = rx.units ? fpSum / rx.units : 0;
    return rx;
}

/* ------------------------------------------------------------------------- */
/* Analysis                                                                      */
/* ------------------------------------------------------------------------- */

struct Metrics {
    std::vector<std::pair<std::string, double>> m;
    std::vector<std::string> flags, info, notes, known;
    void Set(const std::string &k, double v) { for (auto &p : m) if (p.first == k) { p.second = v; return; } m.push_back({ k, v }); }
    double Get(const std::string &k, double d = NAN) const { for (auto &p : m) if (p.first == k) return p.second; return d; }
};

/* Delay of dec against ref: the lag maximising the sum over channels of the per-channel
 * cross-correlation (a channel sum would cancel for opposite-phase stereo). */
static int EstimateDelay(const std::vector<double> &ref, const std::vector<double> &dec, int ch, size_t begin, size_t end, int dmin, int dmax)
{
    size_t frames = ref.size() / ch, dframes = dec.size() / ch;
    if (end > frames) end = frames;
    if (begin >= end) return 0;
    size_t la = end - begin;
    size_t lb = la + (size_t)(dmax - dmin);
    size_t P = 1;
    while (P < la + lb) P <<= 1;
    std::vector<double> corr(P, 0.0);
    for (int c = 0; c < ch; c++) {
        std::vector<std::complex<double>> A(P), B(P);
        for (size_t n = 0; n < la; n++) A[n] = ref[(begin + n) * ch + c];
        for (size_t n = 0; n < lb; n++) {
            long long idx = (long long)begin + dmin + (long long)n;
            B[n] = idx >= 0 && (size_t)idx < dframes ? dec[(size_t)idx * ch + c] : 0.0;
        }
        Fft(A, false); Fft(B, false);
        for (size_t k = 0; k < P; k++) A[k] = std::conj(A[k]) * B[k];
        Fft(A, true);
        for (size_t k = 0; k < P; k++) corr[k] += A[k].real();
    }
    int best = dmin; double bv = -1e300;
    for (int d = 0; d <= dmax - dmin; d++) if (corr[(size_t)d] > bv) { bv = corr[(size_t)d]; best = dmin + d; }
    return best;
}

struct SineFit { double amp, phase, dc, resid, total; };
static SineFit FitSine(const std::vector<double> &x, int ch, int c, size_t begin, size_t end, double w)
{
    double S[3][3] = {}, Y[3] = {};
    for (size_t n = begin; n < end; n++) {
        double v[3] = { cos(w * n), sin(w * n), 1 };
        double y = x[n * ch + c];
        for (int i = 0; i < 3; i++) { Y[i] += v[i] * y; for (int j = 0; j < 3; j++) S[i][j] += v[i] * v[j]; }
    }
    /* Gaussian elimination */
    double M[3][4];
    for (int i = 0; i < 3; i++) { for (int j = 0; j < 3; j++) M[i][j] = S[i][j]; M[i][3] = Y[i]; }
    for (int i = 0; i < 3; i++) {
        int piv = i;
        for (int r = i + 1; r < 3; r++) if (fabs(M[r][i]) > fabs(M[piv][i])) piv = r;
        for (int k = 0; k < 4; k++) std::swap(M[i][k], M[piv][k]);
        if (fabs(M[i][i]) < 1e-300) return { 0, 0, 0, 0, 0 };
        for (int r = 0; r < 3; r++) if (r != i) { double f = M[r][i] / M[i][i]; for (int k = i; k < 4; k++) M[r][k] -= f * M[i][k]; }
    }
    double a = M[0][3] / M[0][0], b = M[1][3] / M[1][1], d = M[2][3] / M[2][2];
    double resid = 0, total = 0;
    for (size_t n = begin; n < end; n++) {
        double y = x[n * ch + c];
        double e = y - (a * cos(w * n) + b * sin(w * n) + d);
        resid += e * e; total += y * y;
    }
    return { sqrt(a * a + b * b), atan2(-b, a), d, resid, total };
}

/* Window energies (mean square over channels) of a signal, 1 ms windows, hop W/2. */
struct Windows { std::vector<double> e; size_t W, hop; };
static Windows WindowEnergy(const std::vector<double> &x, int ch, size_t begin, size_t end, double rate)
{
    Windows w;
    w.W = (size_t)PN_MAX(16.0, floor(rate * 0.001 + 0.5));
    w.hop = w.W / 2;
    for (size_t s = begin; s + w.W <= end; s += w.hop) {
        double acc = 0;
        for (size_t n = s; n < s + w.W; n++) for (int c = 0; c < ch; c++) acc += x[n * ch + c] * x[n * ch + c];
        w.e.push_back(acc / (double)(w.W * ch));
    }
    return w;
}

static std::vector<double> RunningMedian(const std::vector<double> &e, size_t half)
{
    std::vector<double> out(e.size());
    std::vector<double> tmp;
    for (size_t i = 0; i < e.size(); i++) {
        size_t a = i > half ? i - half : 0, b = PN_MIN(e.size(), i + half + 1);
        tmp.assign(e.begin() + a, e.begin() + b);
        std::nth_element(tmp.begin(), tmp.begin() + tmp.size() / 2, tmp.end());
        out[i] = tmp[tmp.size() / 2];
    }
    return out;
}

struct ClickScan { double maxDb = -300; size_t at = 0; double levelDbfs = -300; int count = 0; };
/* Outliers of window energy against the running median (+-50 ms). Ranges in [lo,hi) window
 * indices listed in `skip` are excluded (intentional events are scored separately). */
static ClickScan ScanClicks(const Windows &w, const std::vector<double> &base, size_t from, size_t to,
                            const std::vector<std::pair<size_t, size_t>> &skip)
{
    ClickScan r;
    double floorE = pow(10, TH_CLICK_FLOOR_DBFS / 10);
    bool inClick = false;
    for (size_t i = from; i < to && i < w.e.size(); i++) {
        bool skipped = false;
        for (auto &s : skip) if (i >= s.first && i < s.second) skipped = true;
        if (skipped) { inClick = false; continue; }
        double ratio = w.e[i] / PN_MAX(base[i], floorE);
        double db = Db10(ratio);
        if (w.e[i] < floorE) db = PN_MIN(db, 0.0);
        if (db > r.maxDb) { r.maxDb = db; r.at = i; r.levelDbfs = Db10(w.e[i]); }
        bool over = db > TH_CLICK_DB;
        if (over && !inClick) r.count++;
        inClick = over;
    }
    return r;
}

/* Tone annihilator x[n+1] - 2cos(w)x[n] + x[n-1]: zero for any pure tone of frequency w. */
static std::vector<double> Annihilate(const std::vector<double> &x, int ch, double w)
{
    std::vector<double> y(x.size(), 0);
    size_t frames = x.size() / ch;
    double k = 2 * cos(w);
    for (size_t n = 1; n + 1 < frames; n++) for (int c = 0; c < ch; c++)
        y[n * ch + c] = x[(n + 1) * ch + c] - k * x[n * ch + c] + x[(n - 1) * ch + c];
    return y;
}

/* 4th-order Butterworth high-pass (two RBJ biquads, Q 0.5412 / 1.3066), per channel. */
static std::vector<double> HighPass(const std::vector<double> &x, int ch, double fs, double fc)
{
    std::vector<double> y = x;
    size_t frames = x.size() / ch;
    for (double q : { 0.54119610, 1.30656296 }) {
        double w0 = 2 * PI * fc / fs, al = sin(w0) / (2 * q), cw = cos(w0);
        double b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + al, a1 = -2 * cw, a2 = 1 - al;
        b0 /= a0; b1 /= a0; b2 /= a0; a1 /= a0; a2 /= a0;
        for (int c = 0; c < ch; c++) {
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (size_t n = 0; n < frames; n++) {
                double in = y[n * ch + c];
                double out = b0 * in + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
                x2 = x1; x1 = in; y2 = y1; y1 = out;
                y[n * ch + c] = out;
            }
        }
    }
    return y;
}

struct JobOut {
    Job job;
    std::string config;
    Metrics M;
    StreamResult S;      /* kept for cross-job comparisons (refs dropped after use) */
    int delay = 0;
};

static std::mutex g_Mx;
static std::map<std::string, int> g_Delay;   /* config key -> calibrated codec delay (frames) */


static bool Decode(const ConfigDef &C, const RxResult &rx, const std::string &base, std::vector<float> &out, int &channels, std::string &err)
{
    std::string in, args;
    switch (C.cfg.Codec) {
    case PNA2DP_CODEC_SBC: in = base + ".sbc"; args = "-f sbc"; break;
    case PNA2DP_CODEC_AAC: in = base + ".loas"; args = "-f loas"; break;
    case PNA2DP_CODEC_APTX: case PNA2DP_CODEC_APTX_LL: in = base + ".aptx"; args = Sfmt("-f aptx -sample_rate %u", C.cfg.SampleRate); break;
    case PNA2DP_CODEC_APTX_HD: in = base + ".aptxhd"; args = Sfmt("-f aptx_hd -sample_rate %u", C.cfg.SampleRate); break;
    default: err = "no decoder"; return false;
    }
    channels = C.cfg.Channels;
    out.clear();
    /* a restarted stream is a new stream for the sink: it starts a new decoder */
    size_t split = rx.splitAt && rx.splitAt < rx.stream.size() ? rx.splitAt : 0;
    for (int part = 0; part < (split ? 2 : 1); part++) {
        std::string tag = part ? ".part2" : "";
        std::string pin = in + tag, outPath = base + tag + ".dec.f32", log = base + tag + ".ffmpeg.log";
        const uint8_t *data = rx.stream.data() + (part ? split : 0);
        size_t len = split ? (part ? rx.stream.size() - split : split) : rx.stream.size();
        if (!WriteFileBytes(pin, data, len)) { err = "cannot write " + pin; return false; }
        int rc = Ffmpeg(args + " -i \"" + pin + "\" -f f32le -c:a pcm_f32le -ac " + std::to_string(channels) + " \"" + outPath + "\"", log);
        if (rc) { err = Sfmt("ffmpeg exit %d (see %s)", rc, log.c_str()); return false; }
        std::vector<uint8_t> raw;
        if (!ReadFileBytes(outPath, raw)) { err = "no decoder output"; return false; }
        size_t at = out.size();
        out.resize(at + raw.size() / 4);
        memcpy(out.data() + at, raw.data(), (raw.size() / 4) * 4);
        if (!g_Keep) { DeleteFileW(Widen(pin).c_str()); DeleteFileW(Widen(outPath).c_str()); DeleteFileW(Widen(log).c_str()); }
    }
    return true;
}

/* Places decoded units at their RTP timestamps (a sink without concealment plays silence in a hole). */
static std::vector<double> Timeline(const RxResult &rx, const std::vector<float> &dec, int ch, size_t minFrames, std::string &note)
{
    size_t total = 0;
    for (auto f : rx.decFrames) total += f;
    size_t decFrames = dec.size() / ch;
    size_t frames = PN_MAX((size_t)rx.tlFrames, minFrames) + 8192;
    std::vector<double> tl(frames * ch, 0.0);
    if (decFrames != total) note = Sfmt("decoder produced %zu frames for %zu expected; placed sequentially", decFrames, total);
    size_t cur = 0;
    for (size_t u = 0; u < rx.decFrames.size(); u++) {
        size_t pos = decFrames == total ? (size_t)rx.tlPos[u] : cur;
        for (size_t k = 0; k < rx.decFrames[u] && cur + k < decFrames && pos + k < frames; k++)
            for (int c = 0; c < ch; c++) tl[(pos + k) * ch + c] = dec[(cur + k) * ch + c];
        cur += rx.decFrames[u];
    }
    if (decFrames != total) for (size_t k = cur; k < decFrames && k < frames; k++) for (int c = 0; c < ch; c++) tl[k * ch + c] = dec[k * ch + c];
    return tl;
}

static void AnalyzeConversion(const ConfigDef &C, const Job &J, const StreamResult &S, Metrics &M)
{
    int ch = S.channels;
    size_t frames = S.ref.size() / ch;
    /* clip count of the codec input */
    size_t clip = 0;
    double peak = 0;
    for (double v : S.ref) { peak = PN_MAX(peak, fabs(v)); if (fabs(v) >= 0.99996) clip++; }
    M.Set("ref_peak_dbfs", Db20(peak));
    M.Set("ref_clip_samples", (double)clip);
    if (S.idealValid) {
        size_t n = PN_MIN(frames, S.ideal.size() / ch);
        double maxErr = 0, acc = 0;
        ULONG fmt = C.path == PATH_HOST ? (C.cfg.Codec == PNA2DP_CODEC_AAC ? PNA2DP_FMT_S16 : C.cfg.Codec == PNA2DP_CODEC_LDAC ?
                     (C.cfg.PcmFormat ? C.cfg.PcmFormat : PNA2DP_FMT_S32) : C.cfg.Codec == PNA2DP_CODEC_SBC ? PNA2DP_FMT_S16 : PNA2DP_FMT_S24) : PNA2DP_FMT_S32;
        double lsb = fmt == PNA2DP_FMT_S16 ? 1.0 / 32768 : fmt == PNA2DP_FMT_S24 ? 1.0 / 8388608 : fmt == PNA2DP_FMT_F32 ? 1.0 / 16777216 : 1.0 / 2147483648.0;
        for (size_t i = 0; i < n * ch; i++) {
            double e = S.ref[i] - S.ideal[i];
            maxErr = PN_MAX(maxErr, fabs(e)); acc += e * e;
        }
        double errLsb = maxErr / lsb;
        M.Set("conv_max_err_lsb", errLsb);
        M.Set("conv_err_rms_dbfs", Db10(acc / (double)PN_MAX(n * ch, (size_t)1)));
        /* S16 targets carry TPDF dither (+-1 LSB) plus rounding: 1.5 LSB; others: 1 LSB
         * (gain truncation, 24-bit truncation, float rounding). */
        double allowed = fmt == PNA2DP_FMT_S16 ? 1.51 : 1.01;
        if (fmt == PNA2DP_FMT_S32 && kFmts[J.fmt].isFloat) allowed = 128.0;   /* float has 24-bit mantissa */
        if (errLsb > allowed) M.flags.push_back(Sfmt("CONVERSION: max error %.2f LSB of the encoder format (allowed %.2f)", errLsb, allowed));
    }
}

static void AnalyzeDecimator(const ConfigDef &C, const Job &J, const StreamResult &S, Metrics &M)
{
    if (C.engineRate != 2 * C.cfg.SampleRate) return;
    int ch = S.channels;
    size_t frames = S.ref.size() / ch;
    double fs = C.cfg.SampleRate;
    if (J.sig.kind == K_ALIAS) {
        size_t b = (size_t)(0.2 * fs), e = (size_t)((J.sig.dur - 0.05) * fs);
        double acc = 0;
        for (size_t n = b; n < e && n < frames; n++) for (int c = 0; c < ch; c++) acc += S.ref[n * ch + c] * S.ref[n * ch + c];
        double lvl = Db10(acc / (double)((e - b) * ch)) - Db20(J.sig.amp / sqrt(2.0));
        M.Set("decim_alias_rejection_db", -lvl);
        if (-lvl < 80) M.flags.push_back(Sfmt("DECIMATOR: %.0f Hz above the codec Nyquist only %.1f dB down (spec >= 80 dB)", J.sig.f0, -lvl));
        return;
    }
    if (std::isnan(SigAnalytic(J.sig, 0, 0.3)) || !J.events.empty() || J.volDb != 0) return;
    /* converter output vs the analytic signal 25 codec frames later (51-tap centre at 2:1) */
    size_t b = (size_t)(0.1 * fs), e = (size_t)((J.sig.dur - 0.05) * fs);
    double es = 0, en = 0;
    for (size_t n = b; n < e && n < frames; n++) for (int c = 0; c < ch; c++) {
        double ideal = SigAnalytic(J.sig, c, (n - 25.0) / fs);
        if (ch == 1) ideal = (SigAnalytic(J.sig, 0, (n - 25.0) / fs) + SigAnalytic(J.sig, 1, (n - 25.0) / fs)) / 2;
        double d = S.ref[n * ch + c] - ideal;
        es += ideal * ideal; en += d * d;
    }
    if (es > 0) {
        double snr = Db10(es / PN_MAX(en, 1e-300));
        M.Set("decim_snr_db", snr);
        /* In band (< 0.45 fs_out) the filter ripple is < 0.001 dB: >= 80 dB; sweeps reach the
         * transition band above 0.45 fs_out, so only their level is compared. */
        /* the source quantisation (TPDF-dithered 16/24-bit: LSB^2/4) and the encoder format
         * (S16 with dither: LSB^2/4) bound what the comparison can show */
        double noise = 0;
        if (!kFmts[J.fmt].isFloat && kFmts[J.fmt].valid < 32) noise += pow(2.0, -2.0 * (kFmts[J.fmt].valid - 1)) / 4;
        if (C.path == PATH_HOST && C.cfg.Codec == PNA2DP_CODEC_AAC) noise += pow(2.0, -30.0) / 4;
        double floorSnr = noise > 0 ? Db10(es / (double)((e - b) * ch) / noise) - 3 : 200;
        double need = PN_MIN(floorSnr, J.sig.kind == K_SWEEP ? 20.0 : J.sig.kind == K_IMD || J.sig.f0 > 0.4 * fs ? 40.0 : 80.0);
        if (snr < need) M.flags.push_back(Sfmt("DECIMATOR: converter output only %.1f dB from the ideal (need %.0f)", snr, need));
    }
}

static size_t ActiveEnd(const StreamResult &S)
{
    int ch = PN_MAX(S.channels, 1);
    for (size_t n = S.ref.size() / ch; n > 0; n--) for (int c = 0; c < ch; c++) if (fabs(S.ref[(n - 1) * ch + c]) > 1e-7) return n;
    return 0;
}

static void AnalyzeTonesPreCodec(const Job &J, const StreamResult &S, Metrics &M)
{
    /* discontinuities in the codec input (trims, ramps, starvation, NaN/Inf) */
    if (!SigHasTone(J.sig) || J.sig.kind == K_STEP) return;
    int ch = S.channels;
    double fs = S.codecRate;
    double w = 2 * PI * J.sig.f0 / fs;
    if (J.sig.f0 > 0.45 * fs) return;
    if (ch == 1 && J.sig.kind == K_OPPOSITE) return;    /* mono downmix is silence */
    std::vector<double> y = Annihilate(S.ref, ch, w);
    size_t frames = S.ref.size() / ch;
    size_t b = (size_t)(0.05 * fs), e = PN_MIN(frames, (size_t)((J.sig.dur - 0.01) * fs));
    size_t ae = ActiveEnd(S);
    if (ae > b + (size_t)(0.02 * fs)) e = PN_MIN(e, ae - (size_t)(0.01 * fs));
    Windows wy = WindowEnergy(y, ch, b, e, fs);
    if (wy.e.empty()) return;
    std::vector<double> base = RunningMedian(wy.e, 100);
    double toneDb = Db20(PN_MIN(J.sig.amp, 1.0) / sqrt(2.0)) + (J.sig.kind == K_LONLY || J.sig.kind == K_RONLY ? -3.0103 : 0);
    double worst = -300, worstOver = -300; size_t at = 0;
    for (size_t i = 0; i < wy.e.size(); i++) {
        double v = Db10(wy.e[i]) - toneDb;
        double over = Db10(wy.e[i] / PN_MAX(base[i], 1e-30));
        if (v > TH_DISC_DB && over > TH_CLICK_DB && v > worst) { worst = v; worstOver = over; at = i; }
    }
    double peak = -300;
    for (size_t i = 0; i < wy.e.size(); i++) peak = PN_MAX(peak, Db10(wy.e[i]) - toneDb);
    M.Set("pre_disc_db", peak);
    for (const Mark &mk : S.marks) {
        size_t lo = mk.refFrame > (size_t)(0.01 * fs) ? mk.refFrame - (size_t)(0.01 * fs) : 0, hi = mk.refFrame + (size_t)(0.03 * fs);
        double ev = -300, evOver = -300;
        for (size_t i = 0; i < wy.e.size(); i++) {
            size_t st = b + i * wy.hop;
            if (st >= lo && st < hi) { ev = PN_MAX(ev, Db10(wy.e[i]) - toneDb); evOver = PN_MAX(evOver, Db10(wy.e[i] / PN_MAX(base[i], 1e-30))); }
        }
        M.notes.push_back(Sfmt("pre-codec discontinuity at '%s' (%.3f s): %.1f dB re tone, +%.1f dB over the local floor", mk.label.c_str(), mk.refFrame / fs, ev, evOver));
        if (ev > TH_DISC_DB && evOver > TH_CLICK_DB) {
            /* audio the engine never wrote, or the contrast run without the trim crossfade */
            bool byDesign = mk.label.find("starv") != std::string::npos || mk.label.find("lost") != std::string::npos || J.noCrossfade;
            (byDesign ? M.info : M.flags).push_back(Sfmt("%sCLICK(pre-codec): '%s' at %.3f s: %.1f dB re tone (threshold %.0f)",
                byDesign ? "by design: " : "", mk.label.c_str(), mk.refFrame / fs, ev, TH_DISC_DB));
        }
    }
    if (J.sig.kind == K_NANINF) { M.notes.push_back(Sfmt("NaN/Inf input: worst discontinuity %.1f dB re tone (+%.1f dB over floor) at %.3f s", worst, worstOver, (b + at * wy.hop) / fs)); return; }
    if (worst > -300) {
        bool nearMark = false;
        for (const Mark &mk : S.marks) { double t = (b + at * wy.hop) / fs - mk.refFrame / fs; if (t > -0.02 && t < 0.04) nearMark = true; }
        if (!nearMark) M.flags.push_back(Sfmt("CLICK(pre-codec): discontinuity %.1f dB re tone (+%.1f dB over floor) at %.3f s", worst, worstOver, (b + at * wy.hop) / fs));
    }
}

static void AnalyzeDecoded(const ConfigDef &C, const Job &J, const StreamResult &S, const std::vector<double> &tl, int delay, bool floatDecoder, Metrics &M)
{
    int ch = S.channels;
    double fs = S.codecRate;
    size_t frames = S.ref.size() / ch;
    size_t b = (size_t)(0.1 * fs), e = PN_MIN(frames, (size_t)((J.sig.dur - 0.02) * fs));
    if (J.sig.dur > 3.0) b = (size_t)(0.15 * fs);
    {
        size_t ae = ActiveEnd(S);   /* a latency trim shortens the stream */
        if (J.sig.kind != K_SILENCE && ae > b + (size_t)(0.1 * fs)) e = PN_MIN(e, ae - (size_t)(0.02 * fs));
    }
    /* click and gap verdicts stay below 15 kHz: AAC and low SBC bitpools low-pass the top octave by design */
    /* the abrupt end of the test signal: transform codecs smear it backwards (pre-echo) */
    if (e > b + (size_t)(0.2 * fs)) e -= (size_t)(0.04 * fs);
    size_t eq = e;
    if (J.sig.kind == K_SWEEP) eq = PN_MIN(e, (size_t)(J.sig.dur * log(15000.0 / J.sig.f0) / log(J.sig.f1 / J.sig.f0) * fs));
    bool hfOnly = J.sig.kind == K_IMD || J.sig.kind == K_ALIAS || (SigHasTone(J.sig) && J.sig.f0 > 15000);
    /* aligned decoded copy */
    std::vector<double> d(frames * ch, 0.0);
    size_t tlFrames = tl.size() / ch;
    for (size_t n = 0; n < frames; n++) {
        long long k = (long long)n + delay;
        if (k >= 0 && (size_t)k < tlFrames) for (int c = 0; c < ch; c++) d[n * ch + c] = tl[(size_t)k * ch + c];
    }
    /* full-scale samples in the decoder output (sink clipping) */
    size_t clip = 0, over = 0; double peak = 0, refPeak = 0;
    for (double v : tl) { double a = fabs(v); peak = PN_MAX(peak, a); if (a >= (floatDecoder ? 1.0 : 32767.0 / 32768.0)) clip++; if (a > 1.0) over++; }
    for (double v : S.ref) refPeak = PN_MAX(refPeak, fabs(v));
    M.Set("dec_peak_dbfs", Db20(peak));
    M.Set("dec_clip_samples", (double)clip);
    if (floatDecoder) M.Set("dec_over_fs_samples", (double)over);
    bool aptxFamily = C.cfg.Codec == PNA2DP_CODEC_APTX || C.cfg.Codec == PNA2DP_CODEC_APTX_LL || C.cfg.Codec == PNA2DP_CODEC_APTX_HD;
    if (clip && aptxFamily && J.sig.kind == K_IMD)
        M.info.push_back(Sfmt("codec-inherent: %zu decoded samples clip on -1 dBFS 19+20 kHz (FFmpeg's aptX encoder produces the same count; see baselines)", clip));
    else if (clip) {
        if (Db20(refPeak) < -0.5 && J.sig.kind != K_SQUARE && J.sig.kind != K_IMPULSE)
            M.flags.push_back(Sfmt("CLIP: %zu decoded samples at full scale while the codec input peaks at %.2f dBFS", clip, Db20(refPeak)));
        else M.info.push_back(Sfmt("decoded output reaches full scale on %zu samples (input peak %.2f dBFS)", clip, Db20(refPeak)));
    }
    if (J.sig.kind == K_SILENCE) {
        double mx = 0, acc = 0;
        for (size_t n = 0; n < tlFrames; n++) for (int c = 0; c < ch; c++) { mx = PN_MAX(mx, fabs(tl[n * ch + c])); acc += tl[n * ch + c] * tl[n * ch + c]; }
        M.Set("silence_peak_dbfs", Db20(mx));
        M.Set("silence_rms_dbfs", Db10(acc / (double)PN_MAX(tlFrames * ch, (size_t)1)));
        if (mx > 2.0 / 32768) M.flags.push_back(Sfmt("NOISE: digital silence decodes to %.1f dBFS peak", Db20(mx)));
        return;
    }
    /* per channel LS gain, SNR, level */
    double es = 0, en = 0, er = 0, ed = 0, enRaw = 0;
    std::vector<double> g(ch, 1.0);
    for (int c = 0; c < ch; c++) {
        double xy = 0, xx = 0;
        for (size_t n = b; n < e; n++) { xy += S.ref[n * ch + c] * d[n * ch + c]; xx += S.ref[n * ch + c] * S.ref[n * ch + c]; }
        g[c] = xx > 1e-20 ? xy / xx : 1.0;
    }
    std::vector<double> res(frames * ch, 0.0);
    for (size_t n = 0; n < frames; n++) for (int c = 0; c < ch; c++) res[n * ch + c] = d[n * ch + c] - g[c] * S.ref[n * ch + c];
    if (g_Keep) {
        /* aligned decoded output and codec input, interleaved f32, for offline inspection */
        std::vector<float> a(frames * ch * 2);
        for (size_t i = 0; i < frames * ch; i++) { a[i * 2] = (float)S.ref[i]; a[i * 2 + 1] = (float)d[i]; }
        WriteFileBytes(g_Work + "\\" + Safe(J.id) + ".refdec.f32", a.data(), a.size() * 4);
    }
    for (size_t n = b; n < e; n++) for (int c = 0; c < ch; c++) {
        double r = S.ref[n * ch + c], x = d[n * ch + c];
        es += r * r; ed += x * x; en += res[n * ch + c] * res[n * ch + c]; enRaw += (x - r) * (x - r); er += r * r;
    }
    M.Set("snr_db", Db10(es / PN_MAX(en, 1e-300)));
    M.Set("snr_raw_db", Db10(es / PN_MAX(enRaw, 1e-300)));
    M.Set("level_db", Db10(ed / PN_MAX(er, 1e-300)));
    /* quarter SNRs: a missing or duplicated frame breaks alignment from that point on */
    double qmin = 1e9, qmax = -1e9;
    for (int q = 0; q < 4; q++) {
        size_t qb = b + (e - b) * q / 4, qe = b + (e - b) * (q + 1) / 4;
        double s = 0, nn = 0;
        for (size_t n = qb; n < qe; n++) for (int c = 0; c < ch; c++) { s += S.ref[n * ch + c] * S.ref[n * ch + c]; nn += res[n * ch + c] * res[n * ch + c]; }
        if (s < 1e-12) continue;
        double v = Db10(s / PN_MAX(nn, 1e-300));
        qmin = PN_MIN(qmin, v); qmax = PN_MAX(qmax, v);
    }
    if (qmax > -1e8) { M.Set("snr_quarter_min_db", qmin); M.Set("snr_quarter_max_db", qmax); }
    bool hasEvents = !S.marks.empty();
    if (qmax > -1e8 && qmax - qmin > 20 && J.sig.steady && !hasEvents && J.sig.kind != K_SWEEP)
        M.flags.push_back(Sfmt("ALIGNMENT: SNR varies %.1f..%.1f dB across the stream (missing/duplicated frames or drift)", qmin, qmax));
    /* tone metrics */
    if (SigHasTone(J.sig) && J.sig.kind != K_NANINF) {
        double w = 2 * PI * J.sig.f0 / fs;
        double worstThd = -300, level = 0; int n = 0;
        for (int c = 0; c < ch; c++) {
            if (J.sig.kind == K_LONLY && c == 1 && ch == 2) continue;
            if (J.sig.kind == K_RONLY && c == 0 && ch == 2) continue;
            if (J.sig.kind == K_OPPOSITE && ch == 1) continue;
            SineFit fd = FitSine(d, ch, c, b, e, w);
            SineFit fr = FitSine(S.ref, ch, c, b, e, w);
            double fund = fd.amp * fd.amp / 2 * (double)(e - b);
            double thdn = Db10(fd.resid / PN_MAX(fund, 1e-300));
            double thdnRef = Db10(fr.resid / PN_MAX(fr.amp * fr.amp / 2 * (double)(e - b), 1e-300));
            worstThd = PN_MAX(worstThd, thdn);
            M.Set("thdn_ref_db", thdnRef);
            level += Db20(fd.amp / PN_MAX(fr.amp, 1e-30)); n++;
        }
        if (n) {
            M.Set("thdn_db", worstThd);
            M.Set("tone_level_err_db", level / n);
            if (fabs(level / n) > TH_LEVEL_DB && J.sig.f0 < 15000 && J.sig.amp >= 0.01)
                M.flags.push_back(Sfmt("LEVEL: tone reproduced %+.2f dB off the codec input", level / n));
        }
    }
    /* channel checks */
    if (ch == 2) {
        auto rmsOf = [&](const std::vector<double> &x, int c) { double a = 0; for (size_t k = b; k < e; k++) a += x[k * ch + c] * x[k * ch + c]; return a / (double)(e - b); };
        if (J.sig.kind == K_LONLY || J.sig.kind == K_RONLY) {
            int on = J.sig.kind == K_LONLY ? 0 : 1, off = 1 - on;
            double xt = Db10(rmsOf(d, off) / PN_MAX(rmsOf(S.ref, on), 1e-300));
            M.Set("crosstalk_db", xt);
            if (xt > -40) M.flags.push_back(Sfmt("CHANNEL: %.1f dB of the %s channel appears in the silent channel", xt, on ? "right" : "left"));
            if (rmsOf(d, on) < rmsOf(d, off)) M.flags.push_back("CHANNEL: left/right swapped");
        }
        if (J.sig.kind == K_STEREO2) {
            double wl = 2 * PI * J.sig.f0 / fs, wr = 2 * PI * J.sig.f1 / fs;
            double lL = FitSine(d, ch, 0, b, e, wl).amp, lR = FitSine(d, ch, 0, b, e, wr).amp;
            double rR = FitSine(d, ch, 1, b, e, wr).amp, rL = FitSine(d, ch, 1, b, e, wl).amp;
            M.Set("stereo_sep_l_db", Db20(lR / PN_MAX(lL, 1e-30)));
            M.Set("stereo_sep_r_db", Db20(rL / PN_MAX(rR, 1e-30)));
            if (lR > lL || rL > rR) M.flags.push_back("CHANNEL: left/right swapped");
            else if (Db20(lR / lL) > -40 || Db20(rL / rR) > -40) M.flags.push_back(Sfmt("CHANNEL: stereo separation only %.1f / %.1f dB", Db20(lR / lL), Db20(rL / rR)));
            double bal = Db20(lL / PN_MAX(rR, 1e-30));
            M.Set("balance_db", bal);
            if (fabs(bal) > 0.5) M.flags.push_back(Sfmt("CHANNEL: balance %+.2f dB", bal));
        }
        if (J.sig.kind == K_OPPOSITE) {
            for (int c = 0; c < 2; c++) {
                double xy = 0;
                for (size_t k = b; k < e; k++) xy += d[k * ch + c] * S.ref[k * ch + c];
                if (xy < 0) M.flags.push_back(Sfmt("CHANNEL: %s channel polarity inverted", c ? "right" : "left"));
            }
        }
    }
    /* IMD: difference tone and 2f1-f2 product */
    if (J.sig.kind == K_IMD) {
        double worst = -300, worstRef = -300;
        for (double f : { J.sig.f1 - J.sig.f0, 2 * J.sig.f0 - J.sig.f1 }) {
            if (f <= 0 || f >= fs / 2) continue;
            for (int c = 0; c < ch; c++) {
                worst = PN_MAX(worst, Db20(FitSine(d, ch, c, b, e, 2 * PI * f / fs).amp / J.sig.amp));
                worstRef = PN_MAX(worstRef, Db20(FitSine(S.ref, ch, c, b, e, 2 * PI * f / fs).amp / J.sig.amp));
            }
        }
        M.Set("imd_db", worst); M.Set("imd_ref_db", worstRef);
        double hf = 0, hfr = 0;
        for (int c = 0; c < ch; c++) { hf += FitSine(d, ch, c, b, e, 2 * PI * J.sig.f0 / fs).amp; hfr += FitSine(S.ref, ch, c, b, e, 2 * PI * J.sig.f0 / fs).amp; }
        M.Set("imd_tone_level_db", Db20(hf / PN_MAX(hfr, 1e-30)));
    }
    /* sweep: response and SNR in 40 ms chunks */
    if (J.sig.kind == K_SWEEP) {
        double k = log(J.sig.f1 / J.sig.f0);
        double maxDev = 0, minSnr = 1e9, minSnrF = 0, rolloff = 0;
        size_t chunk = (size_t)(0.04 * fs);
        for (size_t s = b; s + chunk <= e; s += chunk) {
            double tc = (s + chunk / 2.0) / fs, f = J.sig.f0 * exp(tc * k / J.sig.dur);
            double sr = 0, sd = 0, sn = 0;
            for (size_t n = s; n < s + chunk; n++) for (int c = 0; c < ch; c++) { sr += S.ref[n * ch + c] * S.ref[n * ch + c]; sd += d[n * ch + c] * d[n * ch + c]; sn += res[n * ch + c] * res[n * ch + c]; }
            if (sr < 1e-12) continue;
            double resp = Db10(sd / sr), snr = Db10(sr / PN_MAX(sn, 1e-300));
            if (f >= 40 && f <= 15000) { if (fabs(resp) > fabs(maxDev)) maxDev = resp; if (snr < minSnr) { minSnr = snr; minSnrF = f; } }
            if (f > 15000 && f <= 20000) rolloff = PN_MIN(rolloff, resp);
        }
        M.Set("sweep_resp_dev_40_15k_db", maxDev);
        M.Set("sweep_min_snr_40_15k_db", minSnr);
        M.Set("sweep_min_snr_hz", minSnrF);
        M.Set("sweep_resp_min_15k_20k_db", rolloff);
        if (fabs(maxDev) > 3) M.flags.push_back(Sfmt("RESPONSE: %+.1f dB deviation between 40 Hz and 15 kHz", maxDev));
    }
    /* step: pre-echo energy before the onset relative to the tone after it */
    if (J.sig.kind == K_STEP) {
        size_t on = (size_t)(J.sig.f1 * fs);
        double pre = 0, post = 0;
        for (size_t n = on - (size_t)(0.02 * fs); n < on; n++) for (int c = 0; c < ch; c++) pre += d[n * ch + c] * d[n * ch + c];
        for (size_t n = on + (size_t)(0.05 * fs); n < on + (size_t)(0.07 * fs); n++) for (int c = 0; c < ch; c++) post += d[n * ch + c] * d[n * ch + c];
        M.Set("step_preecho_db", Db10(pre / PN_MAX(post, 1e-300)));
    }
    /* click / crackle: residual window energy against the running median */
    Windows wr = WindowEnergy(res, ch, b, eq, fs);
    std::vector<double> base = RunningMedian(wr.e, 100);
    std::vector<std::pair<size_t, size_t>> skip;
    for (const Mark &mk : S.marks) {
        /* a lost block disturbs the decoder for its whole delay line (AAC: two access units) */
        double before = mk.label.find("lost") != std::string::npos ? 0.1 : 0.02, after = mk.label.find("lost") != std::string::npos ? 0.1 : 0.04;
        size_t lo = mk.refFrame > b + (size_t)(before * fs) ? (mk.refFrame - b - (size_t)(before * fs)) / wr.hop : 0;
        size_t hi = mk.refFrame + (size_t)(after * fs) > b ? (mk.refFrame + (size_t)(after * fs) - b) / wr.hop : 0;
        skip.push_back({ lo, hi });
    }
    ClickScan cs = ScanClicks(wr, base, 0, wr.e.size(), skip);
    M.Set("click_max_db", cs.maxDb);
    M.Set("click_count", cs.count);
    M.Set("click_worst_t_s", (b + cs.at * wr.hop) / fs);
    M.Set("click_worst_level_dbfs", cs.levelDbfs);
    /* Clicks are broadband: a discontinuity puts most of its energy above 3 kHz, while a
     * perceptual codec's frame-to-frame noise modulation on a low tone stays near the tone. */
    std::vector<double> resHf = HighPass(res, ch, fs, 3000);
    Windows wh = WindowEnergy(resHf, ch, b, eq, fs);
    std::vector<double> baseHf = RunningMedian(wh.e, 100);
    ClickScan ch3 = ScanClicks(wh, baseHf, 0, wh.e.size(), skip);
    M.Set("click_hf_max_db", ch3.maxDb);
    M.Set("click_hf_worst_t_s", (b + ch3.at * wh.hop) / fs);
    M.Set("click_hf_worst_level_dbfs", ch3.levelDbfs);
    bool clickHf = ch3.count > 0, clickWide = cs.maxDb > TH_CLICK_WIDE_DB && cs.levelDbfs > TH_CLICK_FLOOR_DBFS;
    if (J.sig.steady && (clickHf || clickWide) && !hfOnly)
        M.flags.push_back(clickHf ? Sfmt("CLICK: %d HF residual burst(s), worst +%.1f dB over the local codec noise at %.3f s (%.1f dBFS above 3 kHz)",
                                         ch3.count, ch3.maxDb, (b + ch3.at * wh.hop) / fs, ch3.levelDbfs)
                                  : Sfmt("CLICK: wideband residual burst +%.1f dB over the local codec noise at %.3f s (%.1f dBFS)",
                                         cs.maxDb, (b + cs.at * wr.hop) / fs, cs.levelDbfs));
    else if (J.sig.steady && cs.count && !hfOnly)
        M.info.push_back(Sfmt("codec noise modulation: residual +%.1f dB over its local median at %.3f s (%.1f dBFS), not broadband (HF +%.1f dB)",
                              cs.maxDb, (b + cs.at * wr.hop) / fs, cs.levelDbfs, ch3.maxDb));
    for (size_t i = 0; i < S.marks.size(); i++) {
        /* the codec noise may step with the event (gain, bitrate): compare with the larger of
         * the residual medians 50..250 ms before and after it */
        auto medianOf = [&](long long lo, long long hi) {
            std::vector<double> v;
            for (long long k = PN_MAX(lo, 0ll); k < hi && k < (long long)wr.e.size(); k++) v.push_back(wr.e[(size_t)k]);
            if (v.empty()) return 0.0;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        long long hop50 = (long long)(0.05 * fs / wr.hop), hop250 = (long long)(0.25 * fs / wr.hop);
        long long lo = (long long)skip[i].first, hi = (long long)skip[i].second;
        double bl = PN_MAX(medianOf(lo - hop250, lo - hop50 + 1), medianOf(hi + hop50, hi + hop250));
        std::vector<double> evBase(wr.e.size(), bl);
        for (size_t k = 0; k < evBase.size(); k++) evBase[k] = PN_MAX(bl, base[k]);
        ClickScan ev = ScanClicks(wr, evBase, skip[i].first, skip[i].second, {});
        {
            /* the same on the residual above 3 kHz */
            std::vector<double> v1, v2;
            for (long long k = PN_MAX(lo - hop250, 0ll); k < lo - hop50 + 1 && k < (long long)wh.e.size(); k++) v1.push_back(wh.e[(size_t)k]);
            for (long long k = PN_MAX(hi + hop50, 0ll); k < hi + hop250 && k < (long long)wh.e.size(); k++) v2.push_back(wh.e[(size_t)k]);
            double m1 = 0, m2 = 0;
            if (!v1.empty()) { std::nth_element(v1.begin(), v1.begin() + v1.size() / 2, v1.end()); m1 = v1[v1.size() / 2]; }
            if (!v2.empty()) { std::nth_element(v2.begin(), v2.begin() + v2.size() / 2, v2.end()); m2 = v2[v2.size() / 2]; }
            std::vector<double> hb(wh.e.size());
            for (size_t k = 0; k < hb.size(); k++) hb[k] = PN_MAX(PN_MAX(m1, m2), baseHf[k]);
            ClickScan evh = ScanClicks(wh, hb, skip[i].first, skip[i].second, {});
            M.notes.push_back(Sfmt("post-codec HF residual at '%s': +%.1f dB (%.1f dBFS above 3 kHz)", S.marks[i].label.c_str(), evh.maxDb, evh.levelDbfs));
            /* An event-related HF burst counts when it is an outlier AND within 40 dB of the
             * programme level (TH_DISC_DB); quieter transients right at a gain/bitrate change are
             * covered by the change itself (temporal masking) and are reported as info. */
            double progDb = Db10(es / (double)PN_MAX((e - b) * ch, (size_t)1));
            bool hfClick = evh.maxDb > TH_CLICK_DB && evh.levelDbfs - progDb > TH_DISC_DB;
            /* aptX (sub-band ADPCM) resuming after digital silence: its quantiser steps adapt up
             * from their minimum during the fade-in, an onset error FFmpeg's own aptX HD encoder
             * reproduces to 0.1 dB on the same codec input (the pause-race jobs, 50 ms of silence).
             * At least 30 dB below the programme it is codec-inherent: info, not a click. */
            bool adpcm = C.cfg.Codec == PNA2DP_CODEC_APTX || C.cfg.Codec == PNA2DP_CODEC_APTX_HD || C.cfg.Codec == PNA2DP_CODEC_APTX_LL;
            if (hfClick && adpcm && S.marks[i].label.find("resume: fade-in") != std::string::npos &&
                evh.levelDbfs - progDb <= -30.0 && ev.levelDbfs - progDb <= -30.0) {
                M.info.push_back(Sfmt("aptX onset after silence at '%s': HF residual +%.1f dB over local noise, %.1f dB below the programme (codec-inherent ADPCM adaptation)",
                                      S.marks[i].label.c_str(), evh.maxDb, progDb - evh.levelDbfs));
                hfClick = false;
            }
            if (!hfClick && ev.maxDb <= TH_CLICK_WIDE_DB) {
                if (evh.maxDb > TH_CLICK_DB)
                    M.info.push_back(Sfmt("codec transient at '%s': HF residual +%.1f dB over local noise but %.1f dB below the programme", S.marks[i].label.c_str(), evh.maxDb, progDb - evh.levelDbfs));
                ev.maxDb = PN_MIN(ev.maxDb, TH_CLICK_DB);
            }
            /* a deliberate fade to or from digital silence (pause / resume): a transform codec
             * spreads its quantisation noise around it, so the residual is an outlier against
             * a steady tone's tiny codec noise; it counts as a click when it is not at least
             * 30 dB below the programme (the alternative, no fade, is a full-level step) */
            if (!hfClick && S.marks[i].label.find("fade") != std::string::npos && ev.levelDbfs - progDb <= -30.0 && ev.maxDb > TH_CLICK_DB) {
                M.info.push_back(Sfmt("codec transient at the fade '%s': residual +%.1f dB over local noise, %.1f dB below the programme", S.marks[i].label.c_str(), ev.maxDb, progDb - ev.levelDbfs));
                ev.maxDb = PN_MIN(ev.maxDb, TH_CLICK_DB);
            }
        }
        M.notes.push_back(Sfmt("post-codec residual at '%s': +%.1f dB over local codec noise (%.1f dBFS)", S.marks[i].label.c_str(), ev.maxDb, ev.levelDbfs));
        if (ev.maxDb > TH_CLICK_DB && S.marks[i].label.find("lost") == std::string::npos && S.marks[i].label.find("starv") == std::string::npos &&
            S.marks[i].label.find("trim") == std::string::npos)
            M.flags.push_back(Sfmt("CLICK(codec): '%s': residual +%.1f dB over the local codec noise", S.marks[i].label.c_str(), ev.maxDb));
    }
    /* decoded tone annihilator (catches clicks present in both input and output) */
    if (SigHasTone(J.sig) && J.sig.f0 < 0.45 * fs && J.sig.kind != K_NANINF && !(ch == 1 && J.sig.kind == K_OPPOSITE)) {
        std::vector<double> y = Annihilate(d, ch, 2 * PI * J.sig.f0 / fs);
        Windows wy = WindowEnergy(y, ch, b, e, fs);
        std::vector<double> by = RunningMedian(wy.e, 100);
        ClickScan ca = ScanClicks(wy, by, 0, wy.e.size(), skip);
        M.Set("dec_annihilator_max_db", ca.maxDb);
        double toneDb = Db20(PN_MIN(J.sig.amp, 1.0) / sqrt(2.0)) + (J.sig.kind == K_LONLY || J.sig.kind == K_RONLY ? -3.0103 : 0) + J.volDb;
        M.Set("dec_annihilator_worst_re_tone_db", ca.levelDbfs - toneDb);
        if (J.sig.steady && ca.count && S.marks.empty() && ca.levelDbfs - toneDb > TH_DISC_DB)
            M.flags.push_back(Sfmt("CLICK(decoded tone): +%.1f dB HF burst at %.3f s, %.1f dB re tone", ca.maxDb, (b + ca.at * wy.hop) / fs, ca.levelDbfs - toneDb));
    }
    /* gaps: digital-zero runs and dropouts while the reference is active */
    {
        Windows wref = WindowEnergy(S.ref, ch, b, eq, fs);
        double zero = 1.0 / 65536;
        size_t run = 0, longest = 0, longestAt = 0;
        for (size_t n = b; n < eq; n++) {
            bool z = true;
            for (int c = 0; c < ch; c++) if (fabs(d[n * ch + c]) > zero) z = false;
            size_t wi = (n - b) / wr.hop; if (wi >= wref.e.size()) wi = wref.e.size() ? wref.e.size() - 1 : 0;
            bool active = !wref.e.empty() && wref.e[wi] > pow(10, -50 / 10.0);
            if (z && active) { if (++run > longest) { longest = run; longestAt = n - run + 1; } }
            else run = 0;
        }
        double ms = longest * 1000.0 / fs;
        M.Set("gap_longest_ms", ms);
        size_t drop = 0;
        size_t w5 = (size_t)(0.005 * fs);
        for (size_t s = b; s + w5 <= eq; s += w5) {
            double sr = 0, sd = 0;
            for (size_t n = s; n < s + w5; n++) for (int c = 0; c < ch; c++) { sr += S.ref[n * ch + c] * S.ref[n * ch + c]; sd += d[n * ch + c] * d[n * ch + c]; }
            if (sr / (double)(w5 * ch) > pow(10, -50 / 10.0) && sd < 0.01 * sr) drop++;
        }
        M.Set("dropout_windows_5ms", (double)drop);
        bool intentional = false;
        for (const Mark &mk : S.marks) if (mk.label.find("lost") != std::string::npos || mk.label.find("starv") != std::string::npos || mk.label.find("mute") != std::string::npos) intentional = true;
        if ((ms >= TH_GAP_MS || drop) && !intentional && J.sig.kind != K_IMPULSE && !hfOnly)
            M.flags.push_back(Sfmt("GAP: %.2f ms of digital silence at %.3f s, %zu dropout window(s)", ms, longestAt / fs, drop));
    }
}

/* ------------------------------------------------------------------------- */
/* Job execution                                                                 */
/* ------------------------------------------------------------------------- */

static std::vector<ConfigDef> g_Cfg;
static PNA2DP_CONFIG g_Pref;

static std::string Safe(const std::string &s)
{
    std::string o;
    for (char c : s) o += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    return o;
}

static JobOut RunJob(const Job &J)
{
    JobOut out;
    out.job = J;
    const ConfigDef &C = g_Cfg[J.cfg];
    out.config = C.key;
    Metrics &M = out.M;
    std::string base = g_Work + "\\" + Safe(J.id);
    /* PATH_FFSBC: FFmpeg's own SBC encoder on the same codec input (baseline) */
    Emu emu(C, J, g_Pref);
    StreamResult S = emu.Run();
    if (!S.error.empty()) { M.flags.push_back("SETUP: " + S.error); out.S = std::move(S); return out; }
    M.Set("bitrate_kbps", S.bitrate / 1000.0);
    if (C.cfg.Codec == PNA2DP_CODEC_SBC) { M.Set("sbc_bitpool", S.sbcBitpool); M.Set("sbc_frame_bytes", S.sbcFrameBytes); M.Set("sbc_fit_bitpool", S.fitBitpool); }
    M.Set("frames_per_packet", S.framesPerPacket);
    if (S.trims) M.Set("trims", S.trims);
    if (S.pauseUnsentFrames >= 0) {
        M.Set("pause_residual_ms", S.pauseResidualFrames * 1000.0 / C.engineRate);
        M.Set("pause_unsent_ms", S.pauseUnsentFrames * 1000.0 / C.engineRate);
        if (S.pauseUnsentFrames) M.flags.push_back(Sfmt("PAUSE: %.1f ms of audio rendered before a pause stays unencoded", S.pauseUnsentFrames * 1000.0 / C.engineRate));
    }
    if (S.ringOverruns) M.flags.push_back(Sfmt("RING: %u overruns", S.ringOverruns));
    if (S.encodeErrors) M.flags.push_back(Sfmt("ENCODER: %u encode errors", S.encodeErrors));
    if (S.hostInvalid) M.flags.push_back(Sfmt("HOST: %u invalid/unmatched results", S.hostInvalid));
    if (S.convShort) M.flags.push_back(Sfmt("CONVERSION: %u short conversions", S.convShort));
    if (S.oversizeDrops) M.flags.push_back(Sfmt("PACKET: %u encoded blocks dropped as larger than the MTU", S.oversizeDrops));
    if (S.ldacExtraPcm) M.flags.push_back(Sfmt("LDAC: plugin packet queue overflowed, %u PCM frames of dropped packets", S.ldacExtraPcm));
    AnalyzeConversion(C, J, S, M);
    AnalyzeDecimator(C, J, S, M);
    AnalyzeTonesPreCodec(J, S, M);
    if (J.selftest >= 1 && J.selftest <= 3 && S.pkts.size() > 20) {
        size_t k = S.pkts.size() * 2 / 5;
        while (k < S.pkts.size() && S.pkts[k].fragIndex) k++;
        if (J.selftest == 1) S.pkts.erase(S.pkts.begin() + (ptrdiff_t)k);
        else if (J.selftest == 2) S.pkts.insert(S.pkts.begin() + (ptrdiff_t)k, S.pkts[k]);
        else {
            std::vector<uint8_t> &pb = S.pkts[k].bytes;
            size_t hdr = (CodecUsesRtp(C.cfg.Codec) ? 12 : 0) + (CodecUsesPh(C.cfg.Codec) ? 1 : 0);
            /* AAC CBR pads its access units with fill elements: hit the spectral data near the start */
            size_t at = C.cfg.Codec == PNA2DP_CODEC_AAC ? hdr + 16 : hdr + (pb.size() - hdr) / 2;
            for (size_t i = at; i < pb.size() && i < at + 24; i++) pb[i] ^= 0x5A;
        }
    }
    RxResult rx = Receive(C, S);
    M.Set("packets", rx.packets);
    M.Set("packet_max_bytes", rx.maxPacket);
    M.Set("packet_avg_bytes", rx.avgPacket);
    M.Set("codec_frames_per_packet_avg", rx.framesPerPacketAvg);
    double seconds = (double)(S.ref.size() / PN_MAX(S.channels, 1)) / PN_MAX(S.codecRate, 1u);
    M.Set("l2cap_kbps", rx.l2capBytes * 8.0 / PN_MAX(seconds, 1e-9) / 1000.0);
    if (S.sdu && rx.slots) M.Set("baseband_slots_per_s", (double)rx.slots / seconds);
    if (rx.fragmented) M.Set("fragmented_packets", rx.fragmented);
    if (rx.errors) M.flags.push_back(Sfmt("PACKET: %u receiver check failure(s), first: %s", rx.errors, rx.firstError.c_str()));
    /* aptX LL keeps short packets on purpose (PN_APTX_LL_MAX_BLOCKS) */
    if ((C.cfg.Codec == PNA2DP_CODEC_APTX || C.cfg.Codec == PNA2DP_CODEC_APTX_HD) && rx.packets && rx.maxPacket * 10 < C.mtu * 6)
        M.flags.push_back(Sfmt("PACKING: aptX packets use at most %u of %u MTU bytes (blocks sized as 8/12 bytes per 4 frames; libfreeaptx emits 4/6)", rx.maxPacket, C.mtu));
    if (C.cfg.Codec == PNA2DP_CODEC_AAC && rx.fragmented)
        M.flags.push_back(Sfmt("PACKET: %u AAC access units fragmented over several RTP packets (many sinks drop them)", rx.fragmented));
    /* PCM accounting: everything consumed reaches the timeline (minus codec buffering) */
    uint64_t refFrames = S.ref.size() / PN_MAX(S.channels, 1);
    M.Set("timeline_frames", (double)rx.tlFrames);
    M.Set("codec_input_frames", (double)refFrames);
    {
        int64_t missing = (int64_t)refFrames - (int64_t)rx.tlFrames;
        ULONG buffered = C.cfg.Codec == PNA2DP_CODEC_AAC ? 2048 : C.cfg.Codec == PNA2DP_CODEC_LDAC ? 15 * S.pcmPerCodecFrame : 0;
        M.Set("unsent_frames", (double)missing);
        if (missing < 0 || (uint64_t)missing > buffered + (uint64_t)S.framesPerPacket * S.pcmPerCodecFrame)
            M.flags.push_back(Sfmt("PCM ACCOUNTING: codec input %llu frames, RTP timeline %llu frames", (unsigned long long)refFrames, (unsigned long long)rx.tlFrames));
    }
    if (J.repeat) {
        Emu emu2(C, J, g_Pref);
        StreamResult S2 = emu2.Run();
        bool same = S2.pkts.size() == S.pkts.size();
        for (size_t i = 0; same && i < S.pkts.size(); i++) {
            const auto &a = S.pkts[i].bytes, &b2 = S2.pkts[i].bytes;
            same = a.size() == b2.size() && (a.size() <= 12 || memcmp(a.data() + (CodecUsesRtp(C.cfg.Codec) ? 12 : 0), b2.data() + (CodecUsesRtp(C.cfg.Codec) ? 12 : 0), a.size() - (CodecUsesRtp(C.cfg.Codec) ? 12 : 0)) == 0);
        }
        M.Set("deterministic", same ? 1 : 0);
        if (!same) M.flags.push_back("DETERMINISM: the same input produced different packets");
    }
    if (!C.decodable) {
        if (C.cfg.Codec == PNA2DP_CODEC_LDAC) {
            M.Set("ldac_frame_bytes_min", rx.ldacFrameBytesMin == 0xffffffff ? 0 : rx.ldacFrameBytesMin);
            M.Set("ldac_frame_bytes_max", rx.ldacFrameBytesMax);
            M.Set("ldac_frames", rx.codecFrames);
        }
        out.S = std::move(S);
        return out;
    }
    std::vector<float> dec;
    int dch = 0;
    std::string err;
    if (!Decode(C, rx, base, dec, dch, err)) { M.flags.push_back("DECODE: " + err); out.S = std::move(S); return out; }
    std::vector<float> ignore;
    std::string note;
    std::vector<double> tl = Timeline(rx, dec, dch, (size_t)refFrames, note);
    if (!note.empty()) M.flags.push_back("DECODE: " + note);
    if (dch != S.channels) { M.flags.push_back(Sfmt("DECODE: %d channels decoded, %d encoded", dch, S.channels)); out.S = std::move(S); return out; }
    /* delay: calibrated per configuration on pink noise; verified for broadband signals */
    int dmax = C.cfg.Codec == PNA2DP_CODEC_AAC ? 6000 : 1024;
    int delay;
    double fs = S.codecRate;
    size_t b = (size_t)(0.1 * fs), e = PN_MIN((size_t)refFrames, (size_t)((J.sig.dur - 0.02) * fs));
    bool periodic = SigHasTone(J.sig) || J.sig.kind == K_SQUARE || J.sig.kind == K_IMD || J.sig.kind == K_STEREO2 || J.sig.kind == K_SILENCE || J.sig.kind == K_IMPULSE ||
        J.sig.kind == K_STEP || J.sig.kind == K_ALIAS;
    {
        std::lock_guard<std::mutex> lk(g_Mx);
        auto it = g_Delay.find(C.key);
        delay = it != g_Delay.end() ? it->second : INT_MIN;
    }
    if (J.calibration || delay == INT_MIN) {
        int est = EstimateDelay(S.ref, tl, S.channels, b, e, -64, dmax);
        if (J.calibration) { std::lock_guard<std::mutex> lk(g_Mx); g_Delay[C.key] = est; }
        delay = est;
        M.Set("delay_frames", est);
    } else {
        int est = periodic ? EstimateDelay(S.ref, tl, S.channels, b, e, delay - 3, delay + 3) : EstimateDelay(S.ref, tl, S.channels, b, e, -64, dmax);
        M.Set("delay_frames", est);
        if (est != delay && !periodic && J.sig.kind != K_SILENCE)
            M.flags.push_back(Sfmt("ALIGNMENT: codec delay %d frames, calibrated %d", est, delay));
        if (!periodic) delay = est; else delay = est;
    }
    bool floatDecoder = C.cfg.Codec == PNA2DP_CODEC_AAC;
    if (J.selftest == 4) {
        size_t at = (size_t)(0.7 * fs) + (size_t)PN_MAX(delay, 0);
        if (at * dch < tl.size()) tl[at * dch] += 0.01;   /* one sample, -40 dBFS */
    }
    AnalyzeDecoded(C, J, S, tl, delay, floatDecoder, M);
    if (J.selftest) {
        /* positive controls: the injected defect must be reported */
        const char *want[4][3] = { { "PACKET", "GAP", nullptr }, { "PACKET", nullptr, nullptr }, { "CLICK", "GAP", "DECODE" }, { "CLICK", nullptr, nullptr } };
        bool all = true;
        std::string seen;
        for (int w = 0; w < 3 && want[J.selftest - 1][w]; w++) {
            bool any = false;
            for (auto &f : M.flags) if (!f.rfind(want[J.selftest - 1][w], 0)) any = true;
            /* corruption is detected by any of its three consequences */
            if (J.selftest == 3) { if (any) { all = true; break; } all = false; continue; }
            if (J.selftest == 1 && w == 1) { for (auto &f : M.flags) if (!f.rfind("CLICK", 0)) any = true; }
            all = all && any;
        }
        for (auto &f : M.flags) { M.info.push_back("selftest detected: " + f); seen += f.substr(0, f.find(':')) + " "; }
        M.flags.clear();
        M.Set("selftest_detected", all ? 1 : 0);
        if (!all) M.flags.push_back(Sfmt("SELFTEST: injected defect %d not reported (reported: %s)", J.selftest, seen.empty() ? "nothing" : seen.c_str()));
    }
    if (C.stress) {
        std::vector<std::string> keep;
        for (auto &f : M.flags) {
            if (!f.rfind("PACKET", 0) || !f.rfind("DECODE", 0) || !f.rfind("PCM", 0) || !f.rfind("ENCODER", 0) || !f.rfind("CONVERSION", 0)) keep.push_back(f);
            else M.info.push_back("stress (bitpool far below the usable range): " + f);
        }
        M.flags = keep;
    } else if (!C.known.empty()) {
        /* finding #2: a reachable state with a documented limit is reported by name and bounded,
         * not relabelled as a synthetic stress configuration */
        std::vector<std::string> keep;
        for (auto &f : M.flags) {
            if (!f.rfind("PACKET", 0) || !f.rfind("DECODE", 0) || !f.rfind("PCM", 0) || !f.rfind("ENCODER", 0) || !f.rfind("CONVERSION", 0)) keep.push_back(f);
            else M.known.push_back(C.known + ": " + f);
        }
        double thd = M.Get("thdn_db"), snr = M.Get("snr_db"), hf = M.Get("click_hf_max_db");
        if (J.sig.name == "sine997_-6dBFS" && J.events.empty() && thd == thd && thd > C.knownMaxThdn6)
            keep.push_back(Sfmt("KNOWN ISSUE WORSE: %s: THD+N at -6 dBFS %.1f dB, recorded bound %.1f dB", C.known.c_str(), thd, C.knownMaxThdn6));
        if (J.sig.kind == K_PINK && J.events.empty() && snr == snr && snr < C.knownMinPinkSnr)
            keep.push_back(Sfmt("KNOWN ISSUE WORSE: %s: pink SNR %.1f dB, recorded bound %.1f dB", C.known.c_str(), snr, C.knownMinPinkSnr));
        if (J.sig.steady && J.events.empty() && hf == hf && hf > C.knownMaxClickHf)
            keep.push_back(Sfmt("KNOWN ISSUE WORSE: %s: HF residual burst +%.1f dB, recorded bound +%.1f dB", C.known.c_str(), hf, C.knownMaxClickHf));
        M.flags = keep;
    }
    out.delay = delay;
    S.ref.clear(); S.ref.shrink_to_fit(); S.ideal.clear(); S.ideal.shrink_to_fit();
    out.S = std::move(S);
    return out;
}

/* FFmpeg's SBC encoder on the S16-rounded codec input of a kernel SBC job: an independent
 * reference for what SBC noise at a similar bitrate looks like. */
static JobOut RunFfmpegSbcBaseline(const Job &J, ULONG bitrate, const char *codec = "sbc")
{
    JobOut out;
    out.job = J;
    const ConfigDef &C = g_Cfg[J.cfg];
    bool sbc = !strcmp(codec, "sbc");
    out.config = sbc ? "ffmpeg-sbc@" + std::to_string(bitrate / 1000) + "k" : std::string("ffmpeg-") + codec + "@" + std::to_string(C.cfg.SampleRate / 1000) + "k";
    Metrics &M = out.M;
    Emu emu(C, J, g_Pref);
    StreamResult S = emu.Run();
    if (!S.error.empty()) { M.flags.push_back("SETUP: " + S.error); return out; }
    std::string base = g_Work + "\\" + Safe(J.id);
    int rc;
    if (sbc) {
        std::vector<int16_t> pcm(S.ref.size());
        for (size_t i = 0; i < S.ref.size(); i++) { double v = floor(S.ref[i] * 32768 + 0.5); pcm[i] = (int16_t)PN_MAX(-32768.0, PN_MIN(32767.0, v)); }
        if (!WriteFileBytes(base + ".s16", pcm.data(), pcm.size() * 2)) { M.flags.push_back("SETUP: write"); return out; }
        rc = Ffmpeg(Sfmt("-f s16le -ar %u -ac %d -i \"%s.s16\" -c:a sbc -b:a %u -f sbc \"%s.ff.sbc\"", S.codecRate, S.channels, base.c_str(), bitrate, base.c_str()), base + ".enc.log");
        if (rc) { M.flags.push_back("SETUP: ffmpeg sbc encoder failed"); return out; }
        rc = Ffmpeg(Sfmt("-f sbc -i \"%s.ff.sbc\" -f f32le -c:a pcm_f32le \"%s.ff.f32\"", base.c_str(), base.c_str()), base + ".dec.log");
    } else {
        std::vector<float> pcm(S.ref.size());
        for (size_t i = 0; i < S.ref.size(); i++) pcm[i] = (float)S.ref[i];
        if (!WriteFileBytes(base + ".s16", pcm.data(), pcm.size() * 4)) { M.flags.push_back("SETUP: write"); return out; }
        rc = Ffmpeg(Sfmt("-f f32le -ar %u -ac %d -i \"%s.s16\" -c:a %s -f %s \"%s.ff.sbc\"", S.codecRate, S.channels, base.c_str(), codec, codec, base.c_str()), base + ".enc.log");
        if (rc) { M.flags.push_back(Sfmt("SETUP: ffmpeg %s encoder failed", codec)); return out; }
        rc = Ffmpeg(Sfmt("-f %s -sample_rate %u -i \"%s.ff.sbc\" -f f32le -c:a pcm_f32le \"%s.ff.f32\"", codec, S.codecRate, base.c_str(), base.c_str()), base + ".dec.log");
    }
    std::vector<uint8_t> raw;
    if (rc || !ReadFileBytes(base + ".ff.f32", raw)) { M.flags.push_back("SETUP: ffmpeg sbc decode failed"); return out; }
    std::vector<uint8_t> sbcBytes;
    ReadFileBytes(base + ".ff.sbc", sbcBytes);
    if (sbc && sbcBytes.size() >= 4) { sbc_params sp; if (sbc_parse_header(sbcBytes.data(), (ULONG)sbcBytes.size(), &sp)) { M.Set("sbc_bitpool", sp.bitpool); M.Set("sbc_mode", sp.channel_mode); M.Set("bitrate_kbps", sbc_bitrate(&sp) / 1000.0); } }
    std::vector<double> tl(raw.size() / 4);
    for (size_t i = 0; i < tl.size(); i++) { float f; memcpy(&f, &raw[i * 4], 4); tl[i] = f; }
    for (auto ext : { ".s16", ".ff.sbc", ".ff.f32", ".enc.log", ".dec.log" }) if (!g_Keep) DeleteFileW(Widen(base + ext).c_str());
    double fs = S.codecRate;
    size_t refFrames = S.ref.size() / S.channels;
    size_t b = (size_t)(0.1 * fs), e = PN_MIN(refFrames, (size_t)((J.sig.dur - 0.02) * fs));
    int delay = EstimateDelay(S.ref, tl, S.channels, b, e, -64, 2048);
    if (SigHasTone(J.sig)) {
        /* periodic: borrow the delay of the pink run of the same baseline */
        std::lock_guard<std::mutex> lk(g_Mx);
        auto it = g_Delay.find(out.config);
        if (it != g_Delay.end()) delay = EstimateDelay(S.ref, tl, S.channels, b, e, it->second - 3, it->second + 3);
    } else {
        std::lock_guard<std::mutex> lk(g_Mx);
        g_Delay[out.config] = delay;
    }
    M.Set("delay_frames", delay);
    AnalyzeDecoded(C, J, S, tl, delay, false, M);
    /* the baseline documents expected SBC behaviour; its findings are not PN-A2DP defects */
    for (auto &f : M.flags) M.info.push_back("baseline: " + f);
    M.flags.clear();
    out.S.channels = S.channels; out.S.codecRate = S.codecRate;
    return out;
}

/* ------------------------------------------------------------------------- */
/* JSON / report                                                                 */
/* ------------------------------------------------------------------------- */

static std::string Json(const std::string &s)
{
    std::string o = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) o += Sfmt("\\u%04x", c);
        else o += c;
    }
    return o + "\"";
}
static std::string JsonNum(double v)
{
    if (std::isnan(v)) return "null";
    if (std::isinf(v)) return v > 0 ? "1e308" : "-1e308";
    return Sfmt("%.6g", v);
}

/* ------------------------------------------------------------------------- */
/* Matrix                                                                        */
/* ------------------------------------------------------------------------- */

static int AddConfig(ConfigDef c) { g_Cfg.push_back(c); return (int)g_Cfg.size() - 1; }

static bool MakeSbc(const char *key, const char *label, PNA2DP_CONFIG pref, ULONG rate, ULONG mtu, BOOLEAN xq, RadioModel radio,
                    PathKind path, const PnCodecPlugin *api, ULONG engineRate, int *idx, const std::vector<UCHAR> &ie = kSonySbcIe)
{
    ConfigDef c;
    c.key = key; c.label = label; c.path = path; c.api = api; c.mtu = mtu; c.engineRate = engineRate ? engineRate : rate; c.xq = xq; c.radio = radio;
    c.decodable = true;
    if (xq) pref.Flags |= PNA2DP_CFG_SBC_XQ;
    if (!SelectFromIe(A2DP_CODEC_SBC, ie, pref, rate, 16, c.cfg)) { fprintf(stderr, "config %s: selection failed\n", key); return false; }
    *idx = AddConfig(c);
    return true;
}

int wmain(int argc, wchar_t **argv)
{
    std::string pluginDir, filter;
    unsigned threads = PN_MAX(2u, PN_MIN(16u, std::thread::hardware_concurrency()));
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? Narrow(argv[++i]) : std::string(); };
        if (a == L"--plugins") pluginDir = next();
        else if (a == L"--work") g_Work = next();
        else if (a == L"--ffmpeg") g_Ffmpeg = Widen(next());
        else if (a == L"--threads") threads = (unsigned)atoi(next().c_str());
        else if (a == L"--filter") filter = next();
        else if (a == L"--keep") g_Keep = true;
        else { fprintf(stderr, "usage: quality_test --plugins DIR --work DIR [--ffmpeg EXE] [--threads N] [--filter TEXT] [--keep]\n"); return 2; }
    }
    if (pluginDir.empty() || g_Work.empty()) { fprintf(stderr, "--plugins and --work are required\n"); return 2; }
    CreateDirectoryW(Widen(g_Work).c_str(), NULL);
    if (g_Ffmpeg.empty()) {
        wchar_t path[MAX_PATH];
        if (SearchPathW(NULL, L"ffmpeg.exe", NULL, MAX_PATH, path, NULL)) g_Ffmpeg = path;
        else { fprintf(stderr, "ffmpeg.exe not found (pass --ffmpeg)\n"); return 2; }
    }
    /* plugins, loaded exactly as plugin_test.cpp does */
    const PnCodecPlugin *aac = nullptr, *aptx = nullptr, *ldac = nullptr, *sbcPlugin = PnBuiltinSbcPlugin();
    for (const char *name : { "pna2dp-codec-aac.dll", "pna2dp-codec-aptx.dll", "pna2dp-codec-ldac.dll" }) {
        std::wstring path = Widen(pluginDir + "\\" + name);
        wchar_t full[MAX_PATH];
        GetFullPathNameW(path.c_str(), MAX_PATH, full, NULL);
        HMODULE m = LoadLibraryExW(full, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) { fprintf(stderr, "%s: load failed %lu\n", name, GetLastError()); continue; }
        PnCodecPluginGetInfoFn fn = (PnCodecPluginGetInfoFn)GetProcAddress(m, PNA2DP_PLUGIN_ENTRY);
        const PnCodecPlugin *api = fn ? fn() : nullptr;
        if (!api || api->api_version != PNA2DP_PLUGIN_API_VERSION) { fprintf(stderr, "%s: bad ABI\n", name); continue; }
        if (api->codec_mask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_AAC)) aac = api;
        if (api->codec_mask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_APTX)) aptx = api;
        if (api->codec_mask & PNA2DP_CODEC_BIT(PNA2DP_CODEC_LDAC)) ldac = api;
        printf("plugin %s: %s %s\n", name, api->name, api->version);
    }
    g_Pref = DefaultPref();

    /* ---- configurations ---- */
    PNA2DP_CONFIG dual = g_Pref; dual.Flags |= PNA2DP_CFG_DUAL_CHANNEL_PREFER;
    int kSony48, kSony44, kJoint48, kJoint44, kXq3, kXq2, kXqUnknown, kMono, kHost, kBp250, kSonyD96, kSonyD88;
    std::vector<int> kLow;
    MakeSbc("sbc-k-sony-dual53-48k", "kernel SBC, Sony caps, dual channel bp53, 48 kHz, MTU 895", dual, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kSony48);
    MakeSbc("sbc-k-sony-dual53-44k", "kernel SBC, Sony caps, dual channel bp53, 44.1 kHz, MTU 895", dual, 44100, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kSony44);
    MakeSbc("sbc-k-joint53-48k", "kernel SBC, joint stereo bp53, 48 kHz, MTU 895", g_Pref, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kJoint48);
    MakeSbc("sbc-k-joint53-44k", "kernel SBC, joint stereo bp53, 44.1 kHz, MTU 895", g_Pref, 44100, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kJoint44);
    MakeSbc("sbc-k-xq-3dh5-48k", "kernel SBC XQ, Sony caps, 3-DH5 known both ends, 48 kHz, MTU 895", g_Pref, 48000, 895, TRUE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kXq3);
    MakeSbc("sbc-k-xq-2dh5-48k", "kernel SBC XQ, Sony caps, sink features unknown (2-DH5 point), 48 kHz, MTU 895", g_Pref, 48000, 895, TRUE, kRadioRemoteUnknown, PATH_KSBC, nullptr, 0, &kXq2);
    MakeSbc("sbc-k-xq-radiounknown-44k", "kernel SBC XQ, Sony caps, radio unknown, 44.1 kHz, MTU 895", g_Pref, 44100, 895, TRUE, kRadioUnknown, PATH_KSBC, nullptr, 0, &kXqUnknown);
    { PNA2DP_CONFIG m = g_Pref; m.Sbc.ChannelModeMask = PNA2DP_SBC_CH_MONO;
      MakeSbc("sbc-k-mono53-48k", "kernel SBC mono bp53, 48 kHz (stereo engine downmixed)", m, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kMono); }
    for (int bp : { 2, 8, 12, 16, 20 }) {
        PNA2DP_CONFIG p = g_Pref; p.Sbc.MaxBitpool = (UCHAR)bp;
        int idx;
        std::string key = "sbc-k-joint-bp" + std::to_string(bp) + "-48k";
        if (MakeSbc(key.c_str(), ("kernel SBC joint stereo low bitpool " + std::to_string(bp)).c_str(), p, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &idx)) {
            g_Cfg[idx].stress = bp < 12;
            kLow.push_back(idx);
        }
    }
    { PNA2DP_CONFIG p = g_Pref; p.Sbc.MaxBitpool = 250; p.Sbc.AllowBitpoolOverride = 1;
      MakeSbc("sbc-k-joint-bp250-48k", "kernel SBC joint stereo bitpool 250 override, 48 kHz, MTU 895", p, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &kBp250); }
    MakeSbc("sbc-host-joint53-48k", "SBC plugin (host ABI, S16 + TPDF dither), joint bp53, 48 kHz, MTU 895", g_Pref, 48000, 895, FALSE, kRadio3Dh5, PATH_HOST, sbcPlugin, 0, &kHost);
    MakeSbc("sbc-k-sony-dual53-96to48", "kernel SBC Sony dual bp53, engine 96 kHz -> 48 kHz half-band decimator", dual, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 96000, &kSonyD96);
    MakeSbc("sbc-k-sony-dual53-88to44", "kernel SBC Sony dual bp53, engine 88.2 kHz -> 44.1 kHz half-band decimator", dual, 44100, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 88200, &kSonyD88);

    std::vector<int> aacCfg, aptxCfg, ldacCfg;
    int kAacD96 = -1, kAptxHdD96 = -1, kAac48 = -1, kAptxHd48 = -1, kLdacHq48 = -1, kLdacHq48s16 = -1, kAptx48 = -1, kAptxLl48 = -1;
    if (aac) {
        for (int vbr = 0; vbr < 2; vbr++) for (ULONG mtu : { 895u, 1017u }) for (ULONG rate : { 44100u, 48000u }) {
            std::vector<UCHAR> ie = { 0xC0, 0x01, 0x8C, (UCHAR)((vbr ? 0x80 : 0) | 0x04), 0xE2, 0x00 };
            ConfigDef c;
            c.key = Sfmt("aac-%s-%u-%uk", vbr ? "vbr" : "cbr", mtu, rate / 1000);
            c.label = Sfmt("AAC-LC %s, MTU %u, %.1f kHz (fdk-aac plugin)", vbr ? "VBR" : "CBR", mtu, rate / 1000.0);
            c.path = PATH_HOST; c.api = aac; c.mtu = mtu; c.engineRate = rate; c.xq = FALSE; c.radio = kRadio3Dh5; c.decodable = true;
            if (!SelectFromIe(A2DP_CODEC_MPEG24_AAC, ie, g_Pref, rate, 16, c.cfg)) { fprintf(stderr, "AAC selection failed\n"); continue; }
            int idx = AddConfig(c);
            aacCfg.push_back(idx);
            if (!vbr && mtu == 895 && rate == 48000) {
                kAac48 = idx;
                c.key = "aac-cbr-895-96to48"; c.label = "AAC-LC CBR, MTU 895, engine 96 kHz -> 48 kHz decimator"; c.engineRate = 96000;
                kAacD96 = AddConfig(c);
            }
        }
    }
    if (aptx) {
        struct { ULONG codec; const char *name; std::vector<UCHAR> ie; } fam[] = {
            { PNA2DP_CODEC_APTX, "aptx", { 0x4F, 0, 0, 0, 0x01, 0x00, 0x32 } },
            { PNA2DP_CODEC_APTX_HD, "aptxhd", { 0xD7, 0, 0, 0, 0x24, 0x00, 0x32, 0, 0, 0, 0 } },
            { PNA2DP_CODEC_APTX_LL, "aptxll", { 0x0A, 0, 0, 0, 0x02, 0x00, 0x32, 0x00 } },
        };
        for (auto &f : fam) for (ULONG rate : { 44100u, 48000u }) {
            ConfigDef c;
            c.key = Sfmt("%s-%uk", f.name, rate / 1000);
            c.label = Sfmt("%s %.1f kHz, MTU 895 (libfreeaptx plugin)", A2dpCodecName(f.codec), rate / 1000.0);
            c.path = PATH_HOST; c.api = aptx; c.mtu = 895; c.engineRate = rate; c.xq = FALSE; c.radio = kRadio3Dh5; c.decodable = true;
            if (!SelectFromIe(A2DP_CODEC_VENDOR, f.ie, g_Pref, rate, 24, c.cfg)) { fprintf(stderr, "aptX selection failed\n"); continue; }
            int idx = AddConfig(c);
            aptxCfg.push_back(idx);
            if (f.codec == PNA2DP_CODEC_APTX && rate == 48000) kAptx48 = idx;
            if (f.codec == PNA2DP_CODEC_APTX_LL && rate == 48000) kAptxLl48 = idx;
            if (f.codec == PNA2DP_CODEC_APTX_HD && rate == 48000) {
                kAptxHd48 = idx;
                c.key = "aptxhd-96to48"; c.label = "aptX HD, engine 96 kHz -> 48 kHz decimator"; c.engineRate = 96000;
                kAptxHdD96 = AddConfig(c);
            }
        }
    }
    if (ldac) {
        std::vector<UCHAR> ie = { 0x2D, 0x01, 0, 0, 0xAA, 0x00, 0x3C, 0x07 };
        struct { const char *name; UCHAR eqmid; ULONG rate; ULONG fmtMask; ULONG mtu; } ld[] = {
            { "ldac-hq-48k", PNA2DP_LDAC_EQMID_HQ, 48000, PNA2DP_FMT_S32, 895 },
            { "ldac-sq-48k", PNA2DP_LDAC_EQMID_SQ, 48000, PNA2DP_FMT_S32, 895 },
            { "ldac-mq-48k", PNA2DP_LDAC_EQMID_MQ, 48000, PNA2DP_FMT_S32, 895 },
            { "ldac-hq-44k", PNA2DP_LDAC_EQMID_HQ, 44100, PNA2DP_FMT_S32, 895 },
            { "ldac-hq-96k", PNA2DP_LDAC_EQMID_HQ, 96000, PNA2DP_FMT_S32, 895 },
            { "ldac-hq-48k-mtu679", PNA2DP_LDAC_EQMID_HQ, 48000, PNA2DP_FMT_S32, 679 },
            { "ldac-hq-48k-s16", PNA2DP_LDAC_EQMID_HQ, 48000, PNA2DP_FMT_S16, 895 },
        };
        for (auto &l : ld) {
            PNA2DP_CONFIG p = g_Pref; p.Ldac.Eqmid = l.eqmid; p.Ldac.FormatMask = (UCHAR)l.fmtMask;
            ConfigDef c;
            c.key = l.name; c.label = Sfmt("LDAC EQMID %u, %.1f kHz, MTU %u, encoder input %s (libldac plugin; no decoder)", l.eqmid, l.rate / 1000.0, l.mtu, l.fmtMask == PNA2DP_FMT_S16 ? "S16" : "S32");
            c.path = PATH_HOST; c.api = ldac; c.mtu = l.mtu; c.engineRate = l.rate; c.xq = FALSE; c.radio = kRadio3Dh5; c.decodable = false;
            if (!SelectFromIe(A2DP_CODEC_VENDOR, ie, p, l.rate, 24, c.cfg)) { fprintf(stderr, "LDAC selection failed\n"); continue; }
            int idx = AddConfig(c);
            ldacCfg.push_back(idx);
            if (!strcmp(l.name, "ldac-hq-48k")) kLdacHq48 = idx;
            if (!strcmp(l.name, "ldac-hq-48k-s16")) kLdacHq48s16 = idx;
        }
    }
    (void)kLdacHq48s16;

    /* ---- ABR ladders (production A2dpQualityAtLevel, default 128 kbps floor) and floor-rung configs ---- */
    std::string ladderReport;
    int kSonyFloor = -1, kJointFloor = -1, kAacFloor = -1;
    {
        Job dummy; dummy.cfg = 0; dummy.sig = FindSig("silence"); dummy.fmt = FMT_S16;
        std::vector<int> ladderCfgs = { kSony48, kSony44, kJoint48, kXq3 };
        if (kAac48 >= 0) ladderCfgs.push_back(kAac48);
        if (kLdacHq48 >= 0) ladderCfgs.push_back(kLdacHq48);
        for (int c : ladderCfgs) {
            Emu e(g_Cfg[c], dummy, g_Pref);
            auto lad = e.Ladder();
            ladderReport += Sfmt("    %-24s", g_Cfg[c].key.c_str());
            for (size_t l = 0; l < lad.size(); l++)
                ladderReport += g_Cfg[c].cfg.Codec == PNA2DP_CODEC_AAC ? Sfmt(" L%zu %uk", l, lad[l].second / 1000) :
                                g_Cfg[c].cfg.Codec == PNA2DP_CODEC_LDAC ? Sfmt(" L%zu eq%u/%uk", l, lad[l].first, lad[l].second / 1000) :
                                Sfmt(" L%zu bp%u/%uk", l, lad[l].first, lad[l].second / 1000);
            ladderReport += "\n";
            if (lad.empty()) continue;
            if (c == kSony48 || c == kJoint48) {
                PNA2DP_CONFIG p = c == kSony48 ? dual : g_Pref;
                p.Sbc.MaxBitpool = (UCHAR)lad.back().first;
                int idx;
                std::string key = c == kSony48 ? "sbc-k-sony-dual-abrfloor-48k" : "sbc-k-joint-abrfloor-48k";
                if (MakeSbc(key.c_str(), Sfmt("kernel SBC at the ABR floor rung (bitpool %u, %u kbps)", lad.back().first, lad.back().second / 1000).c_str(),
                            p, 48000, 895, FALSE, kRadio3Dh5, PATH_KSBC, nullptr, 0, &idx)) {
                    (c == kSony48 ? kSonyFloor : kJointFloor) = idx;
                    /* Production reaches this rung (dual channel, or XQ, under link congestion), so
                     * it is no synthetic stress config (finding #2). Dual channel's floor at bitpool
                     * 8 (#31) is a documented trade (a higher floor brings trims and gaps instead,
                     * joint stereo needs a reconfigure): reported as a known issue, bounded at the
                     * recorded quality (THD+N -48.4 dB at -6 dBFS, pink SNR 8.4 dB, HF burst +15.6 dB
                     * on the sweep, which FFmpeg's SBC encoder at bitpool 8 reproduces). */
                    if (lad.back().first < 12) {
                        g_Cfg[idx].known = Sfmt("known #31 (dual-channel ABR floor, bitpool %u at %u kbps)", lad.back().first, lad.back().second / 1000);
                        g_Cfg[idx].knownMaxThdn6 = -45.0;
                        g_Cfg[idx].knownMinPinkSnr = 7.0;
                        g_Cfg[idx].knownMaxClickHf = 18.0;
                    }
                }
            }
            if (c == kAac48) {
                ConfigDef a = g_Cfg[c];
                PNA2DP_CONFIG p = g_Pref; p.Aac.BitrateBps = lad.back().second;
                std::vector<UCHAR> ie = { 0xC0, 0x01, 0x8C, 0x04, 0xE2, 0x00 };
                if (SelectFromIe(A2DP_CODEC_MPEG24_AAC, ie, p, 48000, 16, a.cfg)) {
                    a.key = "aac-cbr-895-48k-abrfloor"; a.label = Sfmt("AAC CBR at the ABR floor rung (%u kbps)", lad.back().second / 1000);
                    kAacFloor = AddConfig(a);
                }
            }
        }
    }

    /* ---- jobs ---- */
    std::vector<Job> jobs;
    auto add = [&](const std::string &group, int cfg, const SigDef &sig, int fmt, double vol, std::vector<Event> ev = {}, bool noX = false, int rep = 0) {
        if (cfg < 0) return;
        Job j;
        j.group = group; j.cfg = cfg; j.sig = sig; j.fmt = fmt; j.volDb = vol; j.events = ev; j.noCrossfade = noX; j.repeat = rep;
        j.calibration = sig.kind == K_PINK && fmt == FMT_S16 && vol == 0 && ev.empty() && group == "codec";
        j.id = Sfmt("%s__%s__%s__%+.0fdB", g_Cfg[cfg].key.c_str(), sig.name.c_str(), kFmts[fmt].name, vol);
        if (!ev.empty()) j.id += "__" + group + (noX ? "-noxfade" : "");
        if (!filter.empty() && j.id.find(filter) == std::string::npos && group.find(filter) == std::string::npos) return;
        jobs.push_back(j);
    };
    std::vector<SigDef> core = CoreSignals();
    /* every codec x every core signal (low bitpools: a subset) */
    std::vector<int> full = { kSony48, kSony44, kJoint48, kJoint44, kXq3, kXq2, kXqUnknown, kMono, kBp250, kHost };
    for (int i : aacCfg) full.push_back(i);
    for (int i : aptxCfg) full.push_back(i);
    for (int i : ldacCfg) full.push_back(i);
    for (int c : full) for (auto &s : core) {
        int fmt = FMT_S16;
        if (g_Cfg[c].cfg.Codec == PNA2DP_CODEC_LDAC || (g_Cfg[c].cfg.Codec >= PNA2DP_CODEC_APTX && g_Cfg[c].cfg.Codec <= PNA2DP_CODEC_APTX_LL)) fmt = FMT_S24IN32;
        if (g_Cfg[c].key == "ldac-hq-48k-s16") fmt = FMT_S16;
        if (fmt != FMT_S16 && s.kind == K_PINK) {
            /* calibration job of a hi-res path runs at its native engine format */
            Job j; j.group = "codec"; j.cfg = c; j.sig = s; j.fmt = fmt; j.volDb = 0; j.calibration = true;
            j.id = Sfmt("%s__%s__%s__%+.0fdB", g_Cfg[c].key.c_str(), s.name.c_str(), kFmts[fmt].name, 0.0);
            j.repeat = g_Cfg[c].path == PATH_HOST ? 1 : 0;
            if (filter.empty() || j.id.find(filter) != std::string::npos) jobs.push_back(j);
            continue;
        }
        add("codec", c, s, fmt, 0, {}, false, s.kind == K_PINK && g_Cfg[c].path == PATH_HOST ? 1 : 0);
    }
    for (int c : kLow) for (const char *n : { "pink_-20dBFSrms", "sine997_-6dBFS", "sine997_0dBFS", "bass60_0dBFS", "silence", "L997_R1499" }) add("codec", c, FindSig(n), FMT_S16, 0);
    for (int c : { kSonyFloor, kJointFloor, kAacFloor })
        for (const char *n : { "pink_-20dBFSrms", "sine997_-6dBFS", "sine997_-20dBFS", "sine997_0dBFS", "bass60_0dBFS", "silence", "L997_R1499", "sweep20-20k_-6dBFS" }) add("codec", c, FindSig(n), FMT_S16, 0);
    /* decimator on: same codec, engine at twice the rate */
    for (int c : { kSonyD96, kSonyD88, kAacD96, kAptxHdD96 }) {
        if (c < 0) continue;
        int fmt = g_Cfg[c].cfg.Codec == PNA2DP_CODEC_APTX_HD ? FMT_S24IN32 : FMT_S16;
        for (const char *n : { "pink_-20dBFSrms", "sine997_-1dBFS", "sine997_-6dBFS", "sine997_-60dBFS", "sweep20-20k_-6dBFS", "imd19k+20k", "bass60_0dBFS", "silence", "L997_R1499", "tone20k" }) {
            SigDef s = FindSig(n);
            if (s.kind == K_PINK) { Job j; j.group = "codec"; j.cfg = c; j.sig = s; j.fmt = fmt; j.calibration = true; j.id = Sfmt("%s__%s__%s__%+.0fdB", g_Cfg[c].key.c_str(), s.name.c_str(), kFmts[fmt].name, 0.0); if (filter.empty() || j.id.find(filter) != std::string::npos) jobs.push_back(j); continue; }
            add("decimator", c, s, fmt, 0);
        }
        SigDef al = FindSig("alias");
        if (g_Cfg[c].engineRate == 88200) al.f0 = 27500;
        add("decimator", c, al, fmt, 0);
        for (int f : { FMT_S24IN32, FMT_F32 }) add("decimator", c, FindSig("sine997_-1dBFS"), f, 0);
    }
    /* input formats x volumes through the production conversion */
    for (int c : { kSony48, kAac48, kAptxHd48, kLdacHq48 }) {
        if (c < 0) continue;
        for (int f = 0; f < 5; f++) for (const char *n : { "sine997_-1dBFS", "sine997_-60dBFS", "pink_-20dBFSrms", "sine997_0dBFS" }) {
            if (f == FMT_S16 && c != kAptxHd48 && c != kLdacHq48) continue;   /* already in the codec matrix */
            if (f == FMT_S24IN32 && (c == kAptxHd48 || c == kLdacHq48)) continue;
            add("format", c, FindSig(n), f, 0);
        }
        for (int f : { FMT_S16, FMT_F32, FMT_S24IN32 }) for (double v : { -6.0, -30.0 }) for (const char *n : { "sine997_-1dBFS", "pink_-20dBFSrms", "sine997_-60dBFS" })
            add("volume", c, FindSig(n), f, v);
        add("format", c, FindSig("overrange"), FMT_F32, 0);
        add("format", c, FindSig("naninf"), FMT_F32, 0);
    }
    /* fault injection */
    SigDef ft = FindSig("fault_tone");
    SigDef fp = FindSig("pink_-20dBFSrms"); fp.dur = 2.4; fp.name = "pink_-20dBFSrms_2.4s";
    std::vector<Event> volEv = { { 0.6, EV_VOLUME, -6 }, { 0.9, EV_VOLUME, -30 }, { 1.2, EV_MUTE, 1 }, { 1.5, EV_MUTE, 0 }, { 1.8, EV_VOLUME, 0 } };
    std::vector<Event> trimEv = { { 0.8, EV_STALL, 330 } };
    std::vector<Event> starveEv = { { 1.0, EV_STARVE, 0 } };
    std::vector<Event> dropEv = { { 1.0, EV_HOSTDROP, 0 } };
    std::vector<Event> restartEv = { { 0.8, EV_RESTART, 200 } };
    std::vector<Event> pauseEv = { { 0.9, EV_PAUSE, 0 } };
    for (int c : { kSony48, kHost, kAac48, kAptxHd48, kLdacHq48 }) {
        if (c < 0) continue;
        int fmt = g_Cfg[c].cfg.Codec == PNA2DP_CODEC_SBC || g_Cfg[c].cfg.Codec == PNA2DP_CODEC_AAC ? FMT_S16 : FMT_S24IN32;
        add("fault-volume-ramps", c, ft, fmt, 0, volEv);
        add("fault-trim", c, ft, fmt, 0, trimEv);
        add("fault-trim", c, ft, fmt, 0, trimEv, true);
        add("fault-starvation", c, ft, fmt, 0, starveEv);
        add("fault-restart", c, ft, fmt, 0, restartEv);
        add("fault-pause", c, ft, fmt, 0, pauseEv);
        if (g_Cfg[c].path == PATH_HOST) add("fault-host-drop", c, ft, fmt, 0, dropEv);
    }
    /* findings #1/#7: a pause whose hand-over completes an encoder block, the pump thread taking
     * it before PumpFinishStream (Emu::Pause race), at 16 pause instants 2 ms apart: every
     * residual phase of the 384-frame SBC and 256-frame LDAC blocks, half of AAC's */
    for (int c : { kSony48, kHost, kAac48, kAptx48, kAptxHd48, kAptxLl48, kLdacHq48 }) {
        if (c < 0) continue;
        int fmt = g_Cfg[c].cfg.Codec == PNA2DP_CODEC_SBC || g_Cfg[c].cfg.Codec == PNA2DP_CODEC_AAC ? FMT_S16 : FMT_S24IN32;
        for (int k = 0; k < 16; k++) {
            Job j; j.group = "fault-pause-race"; j.cfg = c; j.sig = ft; j.fmt = fmt; j.volDb = 0;
            j.events = { { 0.9 + 0.002 * k, EV_PAUSE, 1 } };
            j.id = Sfmt("%s__%s__%s__+0dB__fault-pause-race-%02d", g_Cfg[c].key.c_str(), ft.name.c_str(), kFmts[fmt].name, k);
            if (filter.empty() || j.id.find(filter) != std::string::npos) jobs.push_back(j);
        }
    }
    for (int c : { kSony48, kSony44, kJoint48, kSonyD96 }) {
        if (c < 0) continue;
        std::vector<Event> abr = { { 0.5, EV_ABR, 1 }, { 0.8, EV_ABR, 2 }, { 1.1, EV_ABR, 4 }, { 1.4, EV_ABR, 8 }, { 1.7, EV_ABR, 1 }, { 2.0, EV_ABR, 0 } };
        add("fault-sbc-abr", c, ft, FMT_S16, 0, abr);
        add("fault-sbc-abr", c, fp, FMT_S16, 0, abr);
    }
    for (int c : aacCfg) {
        std::vector<Event> abr = { { 0.5, EV_ABR, 1 }, { 0.9, EV_ABR, 2 }, { 1.3, EV_ABR, 3 }, { 1.6, EV_ABR, 4 }, { 1.9, EV_ABR, 0 } };
        add("fault-aac-bitrate", c, ft, FMT_S16, 0, abr);
        if (c == kAac48) add("fault-aac-bitrate", c, fp, FMT_S16, 0, abr);
    }
    for (int c : { kLdacHq48 }) {
        if (c < 0) continue;
        std::vector<Event> abr = { { 0.5, EV_ABR, 1 }, { 0.9, EV_ABR, 2 }, { 1.4, EV_ABR, 0 } };
        add("fault-ldac-eqmid", c, ft, FMT_S24IN32, 0, abr, false, 1);
    }

    /* detector positive controls */
    for (int c : { kSony48, kAac48, kAptxHd48 }) for (int t = 1; t <= 4; t++) {
        if (c < 0) continue;
        Job j; j.group = "selftest"; j.cfg = c; j.sig = FindSig("sine997_-6dBFS"); j.fmt = g_Cfg[c].cfg.Codec == PNA2DP_CODEC_APTX_HD ? FMT_S24IN32 : FMT_S16;
        j.selftest = t;
        j.id = Sfmt("%s__selftest%d-%s", g_Cfg[c].key.c_str(), t, t == 1 ? "drop-packet" : t == 2 ? "duplicate-packet" : t == 3 ? "corrupt-packet" : "click-40dBFS");
        if (filter.empty() || j.id.find(filter) != std::string::npos || std::string("selftest").find(filter) != std::string::npos) jobs.push_back(j);
    }

    /* ---- run: calibration first (codec delays), then everything else ---- */
    std::vector<JobOut> results(jobs.size());
    auto runPhase = [&](bool calibration) {
        std::atomic<size_t> next(0);
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < threads; t++) pool.emplace_back([&]() {
            for (;;) {
                size_t i = next++;
                if (i >= jobs.size()) break;
                if (jobs[i].calibration != calibration) continue;
                results[i] = RunJob(jobs[i]);
            }
        });
        for (auto &t : pool) t.join();
    };
    printf("%zu jobs, %u threads, ffmpeg %s\n", jobs.size(), threads, Narrow(g_Ffmpeg).c_str());
    fflush(stdout);
    DWORD t0 = GetTickCount();
    runPhase(true);
    runPhase(false);
    /* FFmpeg SBC encoder baselines (independent SBC implementation) */
    std::vector<JobOut> baselines;
    if (filter.empty() || std::string("baseline").find(filter) != std::string::npos) {
        for (const char *n : { "pink_-20dBFSrms", "sine997_-6dBFS", "sine997_-1dBFS", "sine997_0dBFS", "sine997_-20dBFS", "sine997_-60dBFS", "bass60_0dBFS", "silence" }) {
            Job j; j.group = "baseline"; j.cfg = kJoint48; j.sig = FindSig(n); j.fmt = FMT_S16;
            j.id = "ffmpeg-sbc-345k__" + j.sig.name;
            baselines.push_back(RunFfmpegSbcBaseline(j, 345000));
        }
        /* FFmpeg's own aptX / aptX HD encoders (libfreeaptx is derived from them) */
        for (int c : aptxCfg) {
            const char *codec = g_Cfg[c].cfg.Codec == PNA2DP_CODEC_APTX_HD ? "aptx_hd" : g_Cfg[c].cfg.Codec == PNA2DP_CODEC_APTX ? "aptx" : nullptr;
            if (!codec) continue;
            for (const char *n : { "pink_-20dBFSrms", "imd19k+20k", "sine997_-1dBFS", "sine997_-20dBFS" }) {
                Job j; j.group = "baseline"; j.cfg = c; j.sig = FindSig(n); j.fmt = FMT_S24IN32;
                j.id = Sfmt("ffmpeg-%s-%uk__%s", codec, g_Cfg[c].cfg.SampleRate / 1000, j.sig.name.c_str());
                baselines.push_back(RunFfmpegSbcBaseline(j, 0, codec));
            }
        }
    }
    DWORD t1 = GetTickCount();

    /* ---- XQ analysis across radios (production selection + packing) ---- */
    std::string xqReport;
    std::vector<std::string> globalFlags;
    {
        struct Row { const char *label; RadioModel radio; BOOLEAN xq; };
        Row rows[] = {
            { "dual (no XQ), 3-DH5 both", kRadio3Dh5, FALSE }, { "XQ, 3-DH5 both", kRadio3Dh5, TRUE },
            { "dual (no XQ), sink unknown", kRadioRemoteUnknown, FALSE }, { "XQ, sink unknown", kRadioRemoteUnknown, TRUE },
            { "dual (no XQ), radio unknown", kRadioUnknown, FALSE }, { "XQ, radio unknown", kRadioUnknown, TRUE },
        };
        ULONG plainBp = 0, plainBps = 0;
        for (ULONG rate : { 48000u, 44100u }) for (auto &r : rows) {
            ConfigDef c;
            PNA2DP_CONFIG p = r.xq ? g_Pref : dual;
            if (r.xq) p.Flags |= PNA2DP_CFG_SBC_XQ;
            c.key = "xq-probe"; c.path = PATH_KSBC; c.api = nullptr; c.mtu = 895; c.engineRate = rate; c.xq = r.xq; c.radio = r.radio; c.decodable = false;
            if (!SelectFromIe(A2DP_CODEC_SBC, kSonySbcIe, p, rate, 16, c.cfg)) continue;
            Job j; j.cfg = 0; j.sig = FindSig("silence"); j.sig.dur = 0.2; j.fmt = FMT_S16;
            Emu e(c, j, g_Pref);
            StreamResult s = e.Run();
            ULONG frameUs = s.pcmPerCodecFrame ? s.pcmPerCodecFrame * 1000000u / rate : 0;
            ULONG sduBytes = 13 + s.framesPerPacket * s.sbcFrameBytes;
            ULONG slots = s.sdu ? PnSduSlots(s.sdu, sduBytes) : 0;
            double slotsPerS = frameUs && s.framesPerPacket ? slots * 1e6 / (frameUs * s.framesPerPacket) : 0;
            xqReport += Sfmt("    %5.1f kHz %-30s bitpool %2u  %3u kbps  frame %3u B  %u frames/packet  SDU %4u B  %s slots/s %.0f\n",
                             rate / 1000.0, r.label, s.sbcBitpool, s.bitrate / 1000, s.sbcFrameBytes, s.framesPerPacket, sduBytes,
                             s.sdu ? "" : "(baseband unknown)", slotsPerS);
            /* rows alternate plain dual / XQ for the same radio: XQ must never encode less */
            if (!r.xq) { plainBp = s.sbcBitpool; plainBps = s.bitrate; }
            else if (s.bitrate < plainBps)
                globalFlags.push_back(Sfmt("SBC XQ: %.1f kHz, %s: XQ bitpool %u (%u kbps) is below plain dual channel bitpool %u (%u kbps) for the same sink, MTU 895 and radio",
                                           rate / 1000.0, r.radio.name, s.sbcBitpool, s.bitrate / 1000, plainBp, plainBps / 1000));
        }
    }

    /* ---- report ---- */
    std::string jsonPath = g_Work + "\\results.json";
    FILE *jf = NULL;
    fopen_s(&jf, jsonPath.c_str(), "wb");
    size_t flagged = 0;
    if (jf) {
        fprintf(jf, "{\n\"thresholds\": {\"click_db\": %g, \"click_floor_dbfs\": %g, \"disc_db\": %g, \"gap_ms\": %g, \"level_db\": %g},\n",
                TH_CLICK_DB, TH_CLICK_FLOOR_DBFS, TH_DISC_DB, TH_GAP_MS, TH_LEVEL_DB);
        fprintf(jf, "\"configs\": [\n");
        for (size_t i = 0; i < g_Cfg.size(); i++) {
            const ConfigDef &c = g_Cfg[i];
            fprintf(jf, "  {\"key\": %s, \"label\": %s, \"codec\": %s, \"rate\": %u, \"channels\": %u, \"mode\": %u, \"bitrate\": %u, \"sbc_bitpool\": %u, \"mtu\": %u, \"engine_rate\": %u, \"ies\": \"",
                    Json(c.key).c_str(), Json(c.label).c_str(), Json(A2dpCodecName(c.cfg.Codec)).c_str(), c.cfg.SampleRate, c.cfg.Channels, c.cfg.ChannelMode,
                    c.cfg.BitrateBps, c.cfg.SbcBitpool, c.mtu, c.engineRate);
            for (int k = 0; k < c.cfg.IesLength; k++) fprintf(jf, "%02x", c.cfg.Ies[k]);
            fprintf(jf, "\"}%s\n", i + 1 < g_Cfg.size() ? "," : "");
        }
        fprintf(jf, "],\n\"xq\": %s,\n\"abr_ladders\": %s,\n\"global_flags\": [", Json(xqReport).c_str(), Json(ladderReport).c_str());
        for (size_t k = 0; k < globalFlags.size(); k++) fprintf(jf, "%s%s", k ? ", " : "", Json(globalFlags[k]).c_str());
        fprintf(jf, "],\n\"jobs\": [\n");
        std::vector<JobOut *> all;
        for (auto &r : results) all.push_back(&r);
        for (auto &r : baselines) all.push_back(&r);
        for (size_t i = 0; i < all.size(); i++) {
            JobOut &r = *all[i];
            if (!r.M.flags.empty()) flagged++;
            fprintf(jf, "  {\"id\": %s, \"group\": %s, \"config\": %s, \"signal\": %s, \"format\": %s, \"volume_db\": %g, \"metrics\": {",
                    Json(r.job.id).c_str(), Json(r.job.group).c_str(), Json(r.config).c_str(), Json(r.job.sig.name).c_str(), Json(kFmts[r.job.fmt].name).c_str(), r.job.volDb);
            for (size_t k = 0; k < r.M.m.size(); k++) fprintf(jf, "%s%s: %s", k ? ", " : "", Json(r.M.m[k].first).c_str(), JsonNum(r.M.m[k].second).c_str());
            fprintf(jf, "}, \"flags\": [");
            for (size_t k = 0; k < r.M.flags.size(); k++) fprintf(jf, "%s%s", k ? ", " : "", Json(r.M.flags[k]).c_str());
            fprintf(jf, "], \"info\": [");
            for (size_t k = 0; k < r.M.info.size(); k++) fprintf(jf, "%s%s", k ? ", " : "", Json(r.M.info[k]).c_str());
            fprintf(jf, "], \"known\": [");
            for (size_t k = 0; k < r.M.known.size(); k++) fprintf(jf, "%s%s", k ? ", " : "", Json(r.M.known[k]).c_str());
            fprintf(jf, "], \"notes\": [");
            for (size_t k = 0; k < r.M.notes.size(); k++) fprintf(jf, "%s%s", k ? ", " : "", Json(r.M.notes[k]).c_str());
            fprintf(jf, "]}%s\n", i + 1 < all.size() ? "," : "");
        }
        fprintf(jf, "]\n}\n");
        fclose(jf);
    }
    printf("ran %zu jobs + %zu baselines in %.1f s; %zu job(s) with defect flags; results: %s\n", jobs.size(), baselines.size(), (t1 - t0) / 1000.0, flagged, jsonPath.c_str());
    printf("SBC XQ vs plain dual channel on the Sony caps (ff ff 02 35), MTU 895:\n%s", xqReport.c_str());
    printf("ABR ladders after stream start (floor %u bps):\n%s", g_Pref.AbrFloorBps, ladderReport.c_str());
    for (auto &r : results) for (auto &f : r.M.known) printf("KNOWN %s: %s\n", r.job.id.c_str(), f.c_str());
    for (auto &r : results) for (auto &f : r.M.flags) printf("FLAG %s: %s\n", r.job.id.c_str(), f.c_str());
    for (auto &f : globalFlags) printf("FLAG global: %s\n", f.c_str());
    return flagged || !globalFlags.empty() ? 1 : 0;
}
