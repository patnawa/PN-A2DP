/*
 * latency_test.cpp - self test of the acoustic latency measurement (tools/common/pnlatency.cpp).
 *
 * The analysis runs on synthetic recordings built here: the chirp schedule, an audio clock
 * that presents frame 0 at a known time, and a microphone track that hears every chirp a known
 * time after the clock says it plays, with noise, missing chirps, clock and packet-timestamp
 * jitter, alias candidates and a delay that changes during the run. Nothing is played or
 * recorded: the only device calls are a read-only list of microphones and two measurement
 * calls whose endpoint ids do not exist (refused before anything is opened).
 *
 * SPDX-License-Identifier: MIT
 */
#define NOMINMAX
#include <stdio.h>
#include <math.h>
#include <random>
#include <algorithm>
#include "pnlatency.h"

static int g_checks = 0, g_fail = 0;
#define CHECK(cond, ...) do { ++g_checks; if (!(cond)) { g_fail++; printf("FAIL %s:%d  ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const double kPi = 3.14159265358979323846;
static const UINT64 kQpc0 = 98765432101234ull;   /* the microphone's first packet, 100 ns units (~114 days of uptime) */

/* The chirp as a continuous function of time, so a synthetic sound can start between samples. */
static double ChirpAt(double t)
{
    const double d = 0.012;
    if (t < 0 || t >= d) return 0;
    double w = 0.5 - 0.5 * cos(2 * kPi * t / d);
    return w * sin(2 * kPi * (1000 * t + 6000 * t * t / (2 * d)));
}

struct Scene {
    UINT32 renderRate = 48000, captureRate = 16000;
    double lagMs = 281;                    /* the sound arrives this long after the clock's presentation time */
    std::vector<double> lagPerChirpMs;     /* overrides lagMs per chirp */
    std::vector<double> gainPerChirp;      /* 0 = this chirp is not heard; default 1 */
    double amplitude = 0.3, noiseRms = 0.003;
    double clockStartMs = 700;             /* the clock presents frame 0 this long after the microphone starts */
    double clockPeriodMs = 10, packetMs = 10;
    double clockJitterMs = 0, packetJitterMs = 0;
    double recordMs = 0;                   /* microphone length; 0 = through the whole run */
    bool clock = true, clockRuns = true;
    struct Sound { double atMs, amplitude; };   /* extra chirp sounds, ms after the microphone starts */
    std::vector<Sound> extra;
    std::vector<double> clicksMs;          /* single-sample clicks */
    double reflectionMs = 0, reflectionGain = 0;   /* a delayed copy of every chirp (a wall) */
    double humAmplitude = 0;               /* 50 Hz mains hum */
    unsigned seed = 1;
};

static pn::LatencyRecording Make(const Scene &s, const pn::LatencyOptions &o)
{
    pn::LatencyRecording r;
    std::mt19937 rng(s.seed);
    std::uniform_real_distribution<double> jitter(-1.0, 1.0);
    std::normal_distribution<double> noise(0.0, s.noiseRms > 0 ? s.noiseRms : 1.0);
    r.renderRate = s.renderRate; r.captureRate = s.captureRate;
    r.chirpFrames = pn::LatencyChirpFrames(o, s.renderRate);
    const double runMs = s.clockStartMs + pn::LatencyTotalFrames(o, s.renderRate) * 1000.0 / s.renderRate;
    if (s.clock) {
        for (double t = s.clockStartMs - 300; t <= runMs; t += s.clockPeriodMs) {
            double frame = !s.clockRuns || t < s.clockStartMs ? 0 : floor((t - s.clockStartMs) * s.renderRate / 1000.0);
            double at = t + s.clockJitterMs * jitter(rng);
            r.clock.push_back({ frame, kQpc0 + (UINT64)llround(at * 10000.0) });
        }
    }
    const double endMs = s.recordMs > 0 ? s.recordMs : runMs + 300;
    const size_t n = (size_t)(endMs * s.captureRate / 1000.0);
    r.capture.assign(n, 0.0f);
    std::vector<double> track(n, 0.0);
    auto addChirp = [&](double atMs, double amp) {
        double start = atMs * s.captureRate / 1000.0;
        for (size_t i = (size_t)std::max(0.0, ceil(start)); i < n && i < start + 0.012 * s.captureRate + 1; ++i)
            track[i] += amp * ChirpAt((i - start) / s.captureRate);
    };
    for (size_t k = 0; k < r.chirpFrames.size(); ++k) {
        double gain = k < s.gainPerChirp.size() ? s.gainPerChirp[k] : 1.0;
        double lag = k < s.lagPerChirpMs.size() ? s.lagPerChirpMs[k] : s.lagMs;
        if (gain > 0) addChirp(s.clockStartMs + r.chirpFrames[k] * 1000.0 / s.renderRate + lag, s.amplitude * gain);
        if (gain > 0 && s.reflectionGain > 0)
            addChirp(s.clockStartMs + r.chirpFrames[k] * 1000.0 / s.renderRate + lag + s.reflectionMs, s.amplitude * gain * s.reflectionGain);
    }
    for (const auto &e : s.extra) addChirp(e.atMs, e.amplitude);
    for (double c : s.clicksMs) { size_t i = (size_t)(c * s.captureRate / 1000.0); if (i < n) track[i] += 0.9; }
    for (size_t i = 0; i < n && s.humAmplitude > 0; ++i) track[i] += s.humAmplitude * sin(2 * kPi * 50.0 * i / s.captureRate);
    for (size_t i = 0; i < n; ++i) r.capture[i] = (float)(track[i] + (s.noiseRms > 0 ? noise(rng) : 0.0));
    const size_t step = (size_t)(s.packetMs * s.captureRate / 1000.0);
    for (size_t i = 0; i < n; i += step) {
        double at = i * 1000.0 / s.captureRate + s.packetJitterMs * jitter(rng);
        r.packets.push_back({ i, kQpc0 + (UINT64)llround(at * 10000.0) });
    }
    return r;
}

static pn::LatencyResult Run(const Scene &s, const pn::LatencyOptions &o = pn::LatencyOptions())
{
    return pn::AnalyzeLatency(Make(s, o), o);
}

static bool Near(double a, double b, double tolerance) { return fabs(a - b) <= tolerance; }
static bool Has(const std::wstring &text, const wchar_t *part) { return text.find(part) != std::wstring::npos; }
static const char *VerdictName(pn::LatencyVerdict v)
{
    switch (v) {
    case pn::LatencyVerdict::Ok: return "Ok";
    case pn::LatencyVerdict::NotHeard: return "NotHeard";
    case pn::LatencyVerdict::Inconsistent: return "Inconsistent";
    case pn::LatencyVerdict::NoClock: return "NoClock";
    case pn::LatencyVerdict::NoRecording: return "NoRecording";
    case pn::LatencyVerdict::Failed: return "Failed";
    case pn::LatencyVerdict::Cancelled: return "Cancelled";
    }
    return "?";
}
static void Show(const char *name, const pn::LatencyResult &r)
{
    printf("  %-34s %-12s late %+8.2f ms, lag %+7.1f, spread %5.2f ms, clear %u, heard %u of %u\n", name, VerdictName(r.verdict),
           r.lateMs, r.lagMs, r.spreadMs, r.clear, r.heard, r.total);
}

static void TestSignal()
{
    std::vector<float> c48 = pn::ChirpSignal(48000), c16 = pn::ChirpSignal(16000), c441 = pn::ChirpSignal(44100);
    CHECK(c48.size() == 576 && c16.size() == 192 && c441.size() == 529, "12 ms of samples (%zu, %zu, %zu)", c48.size(), c16.size(), c441.size());
    float peak = 0;
    for (float v : c48) peak = std::max(peak, fabsf(v));
    CHECK(peak <= 1.0f && peak > 0.9f, "peak near full scale before the amplitude (%.3f)", peak);
    CHECK(!c48.empty() && fabsf(c48.front()) < 1e-6f && fabsf(c48.back()) < 1e-6f, "Hann window: silent first and last sample");
    unsigned crossings = 0;
    for (size_t i = 1; i < c48.size(); ++i) crossings += (c48[i - 1] < 0) != (c48[i] < 0);
    CHECK(crossings >= 86 && crossings <= 106, "1 -> 7 kHz linear sweep: about 96 zero crossings in 12 ms (%u)", crossings);
    unsigned early = 0, late = 0;   /* 1 kHz at the start, 7 kHz at the end */
    for (size_t i = 1; i < 144 && i < c48.size(); ++i) early += (c48[i - 1] < 0) != (c48[i] < 0);
    for (size_t i = 433; i < 576 && i < c48.size(); ++i) late += (c48[i - 1] < 0) != (c48[i] < 0);
    CHECK(late > 3 * early, "frequency rises (first 3 ms %u crossings, last 3 ms %u)", early, late);

    pn::LatencyOptions o;
    std::vector<UINT64> f = pn::LatencyChirpFrames(o, 48000);
    CHECK(f.size() == 8, "8 chirps by default (%zu)", f.size());
    CHECK(f.size() == 8 && f[0] == 144000 && f[1] == 187200 && f[7] == 506400, "3 s lead-in, then the gaps");
    CHECK(pn::LatencyTotalFrames(o, 48000) == 506400 + 576 + 96000, "through the last chirp and a 2 s tail (%llu)",
          (unsigned long long)pn::LatencyTotalFrames(o, 48000));
    pn::LatencyOptions shortTail; shortTail.tailMs = 100;
    CHECK(pn::LatencyTotalFrames(shortTail, 48000) == 506400 + 576 + 96000, "the tail never ends before maxLagMs + 500 ms");
    CHECK(pn::LatencyChirpFrames(o, 44100).size() == 8 && pn::LatencyChirpFrames(o, 44100)[0] == 132300, "frames at 44.1 kHz");

    /* the schedule property that makes one global lag unambiguous */
    bool apart = true, longPairs = true;
    for (size_t i = 0; i < o.spacingMs.size(); ++i) {
        for (size_t j = i + 1; j < o.spacingMs.size(); ++j)
            apart &= (UINT32)abs((int)o.spacingMs[i] - (int)o.spacingMs[j]) >= 4 * o.toleranceMs;
        if (i + 1 < o.spacingMs.size()) longPairs &= (int)(o.spacingMs[i] + o.spacingMs[i + 1]) > o.maxLagMs - o.minLagMs;
    }
    CHECK(apart, "every gap differs from every other by at least 4 x the tolerance");
    CHECK(longPairs, "two consecutive gaps span more than the lag search range");
    UINT32 sum = o.leadInMs;
    for (UINT32 g : o.spacingMs) sum += g;
    CHECK(sum + o.tailMs <= 13000, "the run takes about 13 s (%u ms)", sum + o.tailMs);
}

static void TestKnownLag()
{
    Scene s;
    pn::LatencyResult r = Run(s);
    Show("281 ms, 16 kHz microphone", r);
    CHECK(r.ok(), "clean recording measures");
    CHECK(r.total == 8 && r.clear == 8 && r.heard == 8, "all 8 chirps clear (%u / %u / %u)", r.clear, r.heard, r.total);
    CHECK(Near(r.lateMs, 281, 0.2), "median lateness 281 ms (%.3f)", r.lateMs);
    CHECK(r.spreadMs <= 0.3, "no spread (%.3f)", r.spreadMs);
    CHECK(Near(r.lagMs, 281, (double)pn::LatencyOptions().toleranceMs), "the global lag is the true one (%.1f)", r.lagMs);
    bool timed = r.chirps.size() == 8, rising = true;
    for (size_t k = 0; k < r.chirps.size(); ++k) {
        timed &= r.chirps[k].timed && r.chirps[k].clear && r.chirps[k].heard && r.chirps[k].match > 0.9;
        if (k) rising &= r.chirps[k].presentedMs > r.chirps[k - 1].presentedMs + 800;
    }
    CHECK(timed, "every chirp timed, clear and a near-perfect match");
    CHECK(rising, "presentation times follow the schedule");
    CHECK(r.chirps.size() == 8 && Near(r.chirps[0].presentedMs, 700 + 3000, 0.1), "first chirp presented after the lead-in (%.2f)",
          r.chirps.empty() ? 0.0 : r.chirps[0].presentedMs);
    CHECK(!r.message.empty() && Has(r.message, L"8 of 8"), "success sentence names the chirps heard");

    s.captureRate = 48000;
    r = Run(s);
    Show("281 ms, 48 kHz microphone", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.1), "48 kHz microphone (%.3f)", r.lateMs);

    s = Scene(); s.renderRate = 44100; s.captureRate = 48000; s.lagMs = 150.5;
    r = Run(s);
    Show("150.5 ms, 44.1 kHz out / 48 kHz in", r);
    CHECK(r.ok() && Near(r.lateMs, 150.5, 0.2), "different render and capture rates (%.3f)", r.lateMs);

    /* the MR5 measurements: 267.6 ms late with a 25 ms report, 98.6 ms early with 364 ms */
    s = Scene(); s.lagMs = 267.6;
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, 267.6, 0.2), "MR5 with a 25 ms report (%.3f)", r.lateMs);
    s.lagMs = -98.6;
    r = Run(s);
    Show("-98.6 ms (sound early)", r);
    CHECK(r.ok() && Near(r.lateMs, -98.6, 0.2), "negative lateness: the sound comes before the clock says (%.3f)", r.lateMs);
    CHECK(Near(r.lagMs, -98.6, 1), "the refinement is centred on the chirps, not on a score-plateau edge (%.1f)", r.lagMs);
    s.lagMs = 0;
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, 0, 0.2), "a correct delay measures 0 (%.3f)", r.lateMs);
    s.lagMs = -480;
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, -480, 0.2), "near the early end of the search range (%.3f)", r.lateMs);
    s.lagMs = 1480;
    r = Run(s);
    Show("1480 ms (late end of the range)", r);
    CHECK(r.ok() && Near(r.lateMs, 1480, 0.2), "near the late end of the search range (%.3f)", r.lateMs);

    s = Scene(); s.clockStartMs = 2600; s.lagMs = 330;   /* the Bluetooth stream took 2.6 s to start */
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, 330, 0.2), "late stream start (%.3f)", r.lateMs);
}

static void TestNoiseAndLevels()
{
    Scene s; s.noiseRms = 0.08;
    pn::LatencyResult r = Run(s);
    Show("noise (SNR about 4 dB per sample)", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.5), "noisy room still measures (%.3f, clear %u)", r.lateMs, r.clear);

    s = Scene(); s.noiseRms = 0.002; s.gainPerChirp = {1, 0.05, 1, 0.2, 1, 1, 0.5, 1};
    r = Run(s);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.2), "quiet chirps count as much as loud ones (%u clear)", r.clear);

    s = Scene(); s.gainPerChirp = {1, 1, 1, 0, 1, 1, 1, 1};
    r = Run(s);
    Show("chirp 4 missing", r);
    CHECK(r.ok() && r.clear == 7 && Near(r.lateMs, 281, 0.2), "one missing chirp: 7 of 8 still measure (%u)", r.clear);
    CHECK(r.chirps.size() == 8 && !r.chirps[3].clear && !r.chirps[3].heard && r.chirps[3].match < 0.5, "the missing chirp is not counted");
    CHECK(Has(r.message, L"7 of 8"), "success sentence counts 7 of 8");

    s.gainPerChirp = {0, 1, 1, 1, 1, 1, 1, 0};
    r = Run(s);
    CHECK(r.ok() && r.clear == 6 && Near(r.lateMs, 281, 0.2), "first and last chirp missing: 6 of 8 is enough (%u)", r.clear);

    s.gainPerChirp = {1, 0, 1, 0, 1, 1, 0, 1};
    r = Run(s);
    Show("3 chirps missing", r);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 5, "5 of 8 is not enough (%s, %u)", VerdictName(r.verdict), r.clear);
    CHECK(Has(r.message, L"5 of 8") && Has(r.message, L"volume") && Has(r.message, L"closer"), "asks for volume / a closer microphone");

    /* the microphone stopped after chirp 5 (heard at 700 + 7350 + 281 ms; chirp 6 at 9281 ms) */
    s = Scene(); s.recordMs = 8800;
    pn::LatencyResult early = Run(s);
    CHECK(early.verdict == pn::LatencyVerdict::NotHeard && early.clear == 5, "a recording that ends early is not a result (%s, %u)", VerdictName(early.verdict), early.clear);

    /* a room: a wall reflection 6 ms after the direct sound at 60 % of its level, then a
     * hard surface 2.5 ms after it at 90 %; the direct sound is what is measured */
    s = Scene(); s.reflectionMs = 6; s.reflectionGain = 0.6;
    r = Run(s);
    Show("reflection 6 ms later at 60 %", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.2), "the direct sound, not the reflection (%.3f)", r.lateMs);
    s.reflectionMs = 2.5; s.reflectionGain = 0.9; s.captureRate = 48000;
    r = Run(s);
    Show("reflection 2.5 ms later at 90 %", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.2), "a strong reflection 2.5 ms later is not mistaken for the sound (%.3f)", r.lateMs);

    /* mains hum is outside the chirp's band; it only lowers the normalized match */
    s = Scene(); s.humAmplitude = 0.2;
    r = Run(s);
    Show("50 Hz hum at 0.2", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.2), "hum does not move the result (%.3f)", r.lateMs);
}

static void TestJitter()
{
    Scene s; s.clockJitterMs = 2;
    pn::LatencyResult r = Run(s);
    Show("clock jitter +-2 ms", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 2.5) && r.spreadMs <= 4.5, "clock jitter (%.3f, spread %.2f)", r.lateMs, r.spreadMs);

    s = Scene(); s.packetJitterMs = 1;
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, 281, 1.5) && r.spreadMs <= 2.5, "microphone packet timestamp jitter (%.3f, spread %.2f)", r.lateMs, r.spreadMs);

    s = Scene(); s.clockJitterMs = 3; s.packetJitterMs = 1; s.noiseRms = 0.03; s.seed = 7;
    r = Run(s);
    Show("both jitters and noise", r);
    CHECK(r.ok() && Near(r.lateMs, 281, 4) && r.spreadMs <= 8.5, "jitter on both clocks with noise (%.3f, spread %.2f)", r.lateMs, r.spreadMs);

    s = Scene(); s.clockPeriodMs = 97; s.packetMs = 20;   /* sparse clock samples, long packets */
    r = Run(s);
    CHECK(r.ok() && Near(r.lateMs, 281, 0.2), "sparse clock samples (%.3f)", r.lateMs);
}

static void TestAliases()
{
    /* A lateness beyond one gap: each chirp's own neighbour sits in its search window. The
     * prototype's 1000 ms periodic spacing reported such a lag modulo the period. */
    Scene s; s.lagMs = 1100;
    pn::LatencyResult r = Run(s);
    Show("1100 ms (beyond a gap)", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 1100, 0.2), "a lateness longer than the smallest gap (%.3f)", r.lateMs);
    for (const auto &c : r.chirps) CHECK(!c.heard || Near(c.heardLateMs, 1100, 1), "the wide window never reaches a neighbour (%.1f)", c.heardLateMs);

    /* the last chirp missing: with uniform spacing lags L and L - period would tie at 7 chirps */
    s.gainPerChirp = {1, 1, 1, 1, 1, 1, 1, 0};
    r = Run(s);
    CHECK(r.ok() && r.clear == 7 && Near(r.lateMs, 1100, 0.2), "no alias when the last chirp is missing (%.3f)", r.lateMs);

    /* louder sounds lined up at one wrong lag for three chirps: a periodic echo or another
     * source. The lag that fits all eight wins. */
    pn::LatencyOptions o;
    std::vector<UINT64> f = pn::LatencyChirpFrames(o, 48000);
    f.resize(8);
    s = Scene(); s.lagMs = 281;
    for (int k = 0; k < 3; ++k) s.extra.push_back({ 700 + f[k] * 1000.0 / 48000 + 1000 + 281, 0.6 });
    r = Run(s);
    Show("281 ms with 3 alias candidates", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 281, 0.2), "alias candidates at +1000 ms ignored (%.3f, lag %.1f)", r.lateMs, r.lagMs);
    s = Scene(); s.lagMs = 600;
    for (int k = 0; k < 3; ++k) s.extra.push_back({ 700 + f[k] * 1000.0 / 48000 + 600 - 450, 0.9 });
    r = Run(s);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, 600, 0.2), "alias candidates 450 ms early ignored (%.3f, lag %.1f)", r.lateMs, r.lagMs);
}

/* A driver that applies more delay than the speaker has presents each chirp before it sounds:
 * a Fixed 1000 ms on a speaker of 280 ms makes the sound 720 ms early, outside the default
 * range. The callers move the range for the delay the driver is expected to apply. */
static void TestLagRange()
{
    pn::LatencyOptions o;
    pn::FitLatencyLagRange(o, 280);
    CHECK(o.minLagMs == -500 && o.maxLagMs == 1500, "a usual delay keeps the default range (%d, %d)", o.minLagMs, o.maxLagMs);
    pn::FitLatencyLagRange(o, 480);
    CHECK(o.minLagMs == -500 && o.maxLagMs == 1500, "480 ms keeps it (%d, %d)", o.minLagMs, o.maxLagMs);
    o = pn::LatencyOptions(); pn::FitLatencyLagRange(o, 481);
    CHECK(o.minLagMs == -511 && o.maxLagMs == 1489, "481 ms moves it (%d, %d)", o.minLagMs, o.maxLagMs);
    o = pn::LatencyOptions(); pn::FitLatencyLagRange(o, 600);
    CHECK(o.minLagMs == -630 && o.maxLagMs == 1370, "600 ms (%d, %d)", o.minLagMs, o.maxLagMs);
    o = pn::LatencyOptions(); pn::FitLatencyLagRange(o, 1000);
    CHECK(o.minLagMs == -1030 && o.maxLagMs == 970, "the largest fixed delay (%d, %d)", o.minLagMs, o.maxLagMs);
    pn::LatencyOptions beyond; pn::FitLatencyLagRange(beyond, 6554);
    CHECK(beyond.minLagMs == -1030 && beyond.maxLagMs == 970, "a larger report moves it no further (%d, %d)", beyond.minLagMs, beyond.maxLagMs);
    bool longPairs = true;
    for (size_t i = 0; i + 1 < o.spacingMs.size(); ++i) longPairs &= (int)(o.spacingMs[i] + o.spacingMs[i + 1]) > o.maxLagMs - o.minLagMs;
    CHECK(longPairs, "the moved range still spans less than two consecutive gaps");
    CHECK(pn::LatencyTotalFrames(o, 48000) == pn::LatencyTotalFrames(pn::LatencyOptions(), 48000), "the run is no longer");

    Scene s; s.lagMs = -720;
    pn::LatencyResult r = Run(s);
    Show("-720 ms, default range", r);
    CHECK(!r.ok(), "the default range does not reach 720 ms early (%s)", VerdictName(r.verdict));
    r = Run(s, o);
    Show("-720 ms, range for 1000 ms applied", r);
    CHECK(r.ok() && r.clear == 8 && Near(r.lateMs, -720, 0.2), "Fixed 1000 ms on a 280 ms speaker measures (%s, %.3f)", VerdictName(r.verdict), r.lateMs);
    pn::AppliedDelayReadings fixed;
    for (int k = 0; k < 9; ++k) fixed.Add(1000000, 6, k == 8);
    pn::SpeakerDelayMeasurement m = pn::SpeakerDelayFromRun(r, fixed);
    CHECK(m.ok && m.delayMs == 280, "and gives the speaker's 280 ms (%u, %ls)", m.delayMs, m.message.c_str());
    s.lagMs = -999;   /* a 1 ms speaker */
    r = Run(s, o);
    CHECK(r.ok() && Near(r.lateMs, -999, 0.2), "the early end: a 1 ms speaker (%.3f)", r.lateMs);
    s.lagMs = 0;
    r = Run(s, o);
    CHECK(r.ok() && Near(r.lateMs, 0, 0.2), "a correct 1000 ms (%.3f)", r.lateMs);
    s.lagMs = 940;
    r = Run(s, o);
    CHECK(r.ok() && Near(r.lateMs, 940, 0.2), "the late end (%.3f)", r.lateMs);
    s = Scene(); s.lagMs = 281; s.gainPerChirp = {1, 1, 1, 0, 1, 1, 1, 1};
    r = Run(s, o);
    CHECK(r.ok() && r.clear == 7 && Near(r.lateMs, 281, 0.2), "a moved range still finds a usual lateness (%.3f)", r.lateMs);
}

/* A microphone with several channels: on a two-input interface the microphone may be on input
 * 2, and input 1 holds only its noise floor (review: only channel 0 was analysed). */
static void TestChannels()
{
    pn::LatencyOptions o;
    Scene s;
    pn::LatencyRecording heard = Make(s, o);
    Scene quiet = s; quiet.gainPerChirp.assign(8, 0.0); quiet.seed = 5;
    pn::LatencyRecording noise = Make(quiet, o);
    const std::vector<float> silent(heard.capture.size(), 0.0f);
    unsigned channel = 99;
    pn::LatencyResult r = pn::AnalyzeLatencyChannels(heard, { noise.capture, heard.capture }, o, &channel);
    Show("chirps on channel 2 only", r);
    CHECK(r.ok() && r.clear == 8 && channel == 1 && Near(r.lateMs, 281, 0.2), "the second input is used (%s, channel %u)", VerdictName(r.verdict), channel);
    r = pn::AnalyzeLatencyChannels(heard, { heard.capture, noise.capture }, o, &channel);
    CHECK(r.ok() && channel == 0, "the first input when it hears them (channel %u)", channel);
    r = pn::AnalyzeLatencyChannels(heard, { silent, heard.capture }, o, &channel);
    CHECK(r.ok() && channel == 1, "a muted first input (channel %u)", channel);
    r = pn::AnalyzeLatencyChannels(heard, { heard.capture, heard.capture }, o, &channel);
    CHECK(r.ok() && channel == 0, "a mono microphone on both channels: the first (channel %u)", channel);
    Scene some = s; some.gainPerChirp = {1, 0, 1, 0, 1, 1, 0, 1};
    pn::LatencyRecording partly = Make(some, o);
    r = pn::AnalyzeLatencyChannels(heard, { partly.capture, noise.capture, heard.capture }, o, &channel);
    CHECK(r.ok() && r.clear == 8 && channel == 2, "the channel that heard every chirp (channel %u, %u clear)", channel, r.clear);
    r = pn::AnalyzeLatencyChannels(heard, { noise.capture, partly.capture }, o, &channel);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 5 && channel == 1 && Has(r.message, L"5 of 8"),
          "no channel measures: the one that heard the most (%s, %u, channel %u)", VerdictName(r.verdict), r.clear, channel);
    r = pn::AnalyzeLatencyChannels(heard, { silent, noise.capture }, o, &channel);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && channel == 1 && Has(r.message, L"No chirps were heard"),
          "a live channel's advice, not the silent one's (channel %u): %ls", channel, r.message.c_str());
    r = pn::AnalyzeLatencyChannels(heard, {}, o, &channel);
    CHECK(r.ok() && channel == 0 && Near(r.lateMs, 281, 0.2), "no channels: the recording's own track");
}

static void TestRejections()
{
    Scene s; s.gainPerChirp.assign(8, 0.0); s.noiseRms = 0.05;
    pn::LatencyResult r = Run(s);
    Show("noise only", r);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 0 && r.heard == 0, "noise only is rejected (%s, %u, %u)", VerdictName(r.verdict), r.clear, r.heard);
    CHECK(Has(r.message, L"No chirps were heard") && Has(r.message, L"volume"), "says nothing was heard");

    s.captureRate = 48000; s.seed = 3;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 0, "noise only, 48 kHz (%u)", r.clear);

    s = Scene(); s.gainPerChirp.assign(8, 0.0); s.noiseRms = 0;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 0, "digital silence is rejected");

    s = Scene(); s.gainPerChirp.assign(8, 0.0); s.noiseRms = 0.001;
    for (double t = 1000; t < 14000; t += 333) s.clicksMs.push_back(t);
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::NotHeard && r.clear == 0, "clicks are not chirps (%u clear)", r.clear);

    /* the speaker changed its delay halfway: 280 ms, then 364 ms */
    s = Scene(); s.lagPerChirpMs = {280, 280, 280, 280, 364, 364, 364, 364};
    r = Run(s);
    Show("delay changed mid-run 280 -> 364", r);
    CHECK(r.verdict == pn::LatencyVerdict::Inconsistent, "a delay change during the run is rejected (%s)", VerdictName(r.verdict));
    CHECK(r.heard == 8 && r.clear == 4, "all heard, half at the run's lag (%u heard, %u clear)", r.heard, r.clear);
    CHECK(Has(r.message, L"changed its delay") && Has(r.message, L"84 ms"), "names the cause and the size of the change");

    /* the change after chirp 6, or before chirp 3: six chirps agree, but the other two were
     * heard clearly at one other delay. Not a result (review: 6 of 8 was accepted with the
     * delay before the change; in Fixed mode the driver's applied value does not move). */
    s.lagPerChirpMs = {280, 280, 280, 280, 280, 280, 364, 364};
    r = Run(s);
    Show("delay changed after chirp 6", r);
    CHECK(r.verdict == pn::LatencyVerdict::Inconsistent && r.clear == 6 && r.heard == 8, "two chirps heard at another delay reject a 6 of 8 run (%s, %u clear, %u heard)",
          VerdictName(r.verdict), r.clear, r.heard);
    CHECK(Has(r.message, L"changed its delay") && Has(r.message, L"84 ms"), "6/2: names the size of the change: %ls", r.message.c_str());
    s.lagPerChirpMs = {280, 280, 364, 364, 364, 364, 364, 364};
    r = Run(s);
    Show("delay changed after chirp 2", r);
    CHECK(r.verdict == pn::LatencyVerdict::Inconsistent && r.clear == 6 && Has(r.message, L"84 ms"), "2/6: rejected too (%s, %u clear): %ls",
          VerdictName(r.verdict), r.clear, r.message.c_str());
    /* one stray sound cannot reject a good run, and neither can two that disagree with each other */
    s.lagPerChirpMs = {280, 280, 280, 280, 280, 280, 280, 364};
    r = Run(s);
    CHECK(r.ok() && r.clear == 7 && Near(r.lateMs, 280, 0.2), "a single chirp elsewhere stays a result (%s, %u clear)", VerdictName(r.verdict), r.clear);
    s.lagPerChirpMs = {280, 280, 200, 280, 280, 280, 380, 280};
    r = Run(s);
    CHECK(r.ok() && r.clear == 6 && Near(r.lateMs, 280, 0.2), "two strays 180 ms apart stay a result (%s, %u clear)", VerdictName(r.verdict), r.clear);

    s.lagPerChirpMs = {280, 280, 280, 280, 290, 290, 290, 290};   /* 10 ms: within the accepted spread */
    r = Run(s);
    CHECK(r.ok() && r.spreadMs > 9 && r.spreadMs < 11 && Near(r.lateMs, 285, 0.3), "a small change stays within the spread (%.2f, %.2f)", r.lateMs, r.spreadMs);

    s.lagPerChirpMs = {266, 296, 281, 270, 292, 275, 288, 284};   /* scattered over 30 ms */
    r = Run(s);
    Show("scattered over 30 ms", r);
    CHECK(r.verdict == pn::LatencyVerdict::Inconsistent && r.spreadMs > 20, "a spread above 20 ms is rejected (%s, %.2f)", VerdictName(r.verdict), r.spreadMs);
    CHECK(Has(r.message, L"noise"), "inconsistent: mentions noise");

    s = Scene(); s.clock = false;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::NoClock && !r.message.empty(), "no clock samples (%s)", VerdictName(r.verdict));
    s.clock = true; s.clockRuns = false;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::NoClock, "a clock that never moved (%s)", VerdictName(r.verdict));

    pn::LatencyOptions o;
    pn::LatencyRecording empty = Make(Scene(), o);
    empty.capture.clear(); empty.packets.clear();
    r = pn::AnalyzeLatency(empty, o);
    CHECK(r.verdict == pn::LatencyVerdict::NoRecording && !r.message.empty(), "no microphone samples (%s)", VerdictName(r.verdict));
    pn::LatencyRecording untimed = Make(Scene(), o);
    untimed.packets.clear();
    r = pn::AnalyzeLatency(untimed, o);
    CHECK(r.verdict == pn::LatencyVerdict::NoRecording, "samples without timestamps (%s)", VerdictName(r.verdict));
    pn::LatencyRecording norate = Make(Scene(), o);
    norate.renderRate = 0;
    r = pn::AnalyzeLatency(norate, o);
    CHECK(r.verdict == pn::LatencyVerdict::NoClock, "no render rate (%s)", VerdictName(r.verdict));

    /* a Bluetooth hands-free microphone records at 8 kHz: the 1 -> 7 kHz chirp folds over and
     * the microphone's filter removes half of it, so the run would end as "not heard" */
    s = Scene(); s.captureRate = 8000;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::Failed && Has(r.message, L"8000 Hz") && Has(r.message, L"16 kHz"), "an 8 kHz microphone is refused (%s): %ls",
          VerdictName(r.verdict), r.message.c_str());
    CHECK(!Has(r.message, L"volume"), "not the volume advice");
    s.captureRate = 11025;
    r = Run(s);
    CHECK(r.verdict == pn::LatencyVerdict::Failed && Has(r.message, L"11025 Hz"), "11025 Hz is refused too (%s)", VerdictName(r.verdict));
}

static void TestText()
{
    Scene s; s.gainPerChirp = {1, 1, 0, 1, 1, 1, 1, 1};
    pn::LatencyResult r = Run(s);
    std::wstring t = pn::LatencyReport(r);
    printf("%ls", t.c_str());
    CHECK(Has(t, L"chirp") && Has(t, L"+281.") && Has(t, L"not heard") && Has(t, L"7 of 8"), "report: table, the missing chirp, the summary");
    CHECK(t.find(L'\r') == std::wstring::npos, "report uses \\n only");
    s = Scene(); s.lagPerChirpMs = {280, 280, 280, 280, 364, 364, 364, 364};
    t = pn::LatencyReport(Run(s));
    printf("%ls", t.c_str());
    CHECK(Has(t, L"+364."), "report shows where a moved chirp was heard");
    s = Scene(); s.gainPerChirp.assign(8, 0.0); s.noiseRms = 0.05;
    t = pn::LatencyReport(Run(s));
    CHECK(!Has(t, L"median"), "no median line without a clear chirp");

    pn::LatencyProgress p;
    CHECK(Has(pn::LatencyProgressText(p), L"Starting"), "starting text");
    p.phase = pn::LatencyPhase::Chirps; p.chirp = 3; p.chirps = 8;
    CHECK(Has(pn::LatencyProgressText(p), L"3 of 8"), "chirp text");
    p.phase = pn::LatencyPhase::Analysing;
    CHECK(Has(pn::LatencyProgressText(p), L"nalys"), "analysing text");

    std::vector<pn::CaptureEndpoint> mics = {
        { L"{0.0.1.00000000}.{a}", L"Microphone (USB Audio Device)", false },
        { L"{0.0.1.00000000}.{b}", L"Headset Microphone (EDIFIER MR5)", false },
        { L"{0.0.1.00000000}.{c}", L"Microphone Array (Realtek(R) Audio)", true },
    };
    CHECK(pn::MatchCaptureEndpoint(mics, L"") == 2, "empty name part: the default microphone");
    CHECK(pn::MatchCaptureEndpoint(mics, L"usb") == 0, "a unique part, any case");
    CHECK(pn::MatchCaptureEndpoint(mics, L"microphone") == -2, "an ambiguous part");
    CHECK(pn::MatchCaptureEndpoint(mics, L"microphone (usb audio device)") == 0, "an exact name");
    CHECK(pn::MatchCaptureEndpoint(mics, L"webcam") == -1, "no match");
    mics[2].isDefault = false;
    CHECK(pn::MatchCaptureEndpoint(mics, L"") == 0, "no default: the first");
    CHECK(pn::MatchCaptureEndpoint({}, L"") == -1, "no microphones");
    std::vector<pn::CaptureEndpoint> same = { { L"x", L"Microphone", false }, { L"y", L"Microphone (2)", false } };
    CHECK(pn::MatchCaptureEndpoint(same, L"Microphone") == 0, "an exact name wins over containment");
}

/* The speaker delay a run measures (the panel's "Measure with microphone", pna2dpctl
 * measure-delay): the delay the driver applied while the chirps played, plus how late the sound
 * came. The driver's value must stay the same through the run. */
static pn::AppliedDelayReadings Steady(UINT32 us, ULONG codec = 6 /* LDAC */)
{
    pn::AppliedDelayReadings a;
    a.Add(us, codec, false);                          /* LeadIn */
    for (int k = 0; k < 8; ++k) a.Add(us, codec, false);   /* every chirp */
    a.Add(us, codec, true);                           /* Finishing */
    return a;
}

static void TestSpeakerDelay()
{
    /* the MR5 on LDAC: the 25 ms report is ignored, the driver applies the 200 ms default and the
     * sound comes 81.4 ms later than the clock says; with the 364 ms report, 98.6 ms earlier */
    Scene s; s.lagMs = 81.4;
    pn::LatencyResult r = Run(s);
    pn::SpeakerDelayMeasurement m = pn::SpeakerDelayFromRun(r, Steady(200000));
    CHECK(m.ok && m.delayMs == 281 && Near(m.appliedMs, 200, 1e-9), "200 ms applied, 81.4 ms late: 281 ms (got %u, ok %d)", m.delayMs, m.ok);
    CHECK(m.message == L"Measured 281 ms (8 of 8 chirps, spread " + std::to_wstring((int)floor(r.spreadMs + 0.5)) + L" ms).", "result text: %ls", m.message.c_str());
    s = Scene(); s.lagMs = -98.6;
    m = pn::SpeakerDelayFromRun(Run(s), Steady(364000));
    CHECK(m.ok && m.delayMs == 265, "364 ms applied, 98.6 ms early: 265 ms (got %u)", m.delayMs);

    /* rounding and limits on an exact result */
    pn::LatencyResult ok;
    ok.verdict = pn::LatencyVerdict::Ok; ok.clear = 7; ok.heard = 7; ok.total = 8; ok.spreadMs = 4.4; ok.lateMs = 80.5;
    m = pn::SpeakerDelayFromRun(ok, Steady(200000));
    CHECK(m.ok && m.delayMs == 281 && m.message == L"Measured 281 ms (7 of 8 chirps, spread 4 ms).", "280.5 rounds up: %u, %ls", m.delayMs, m.message.c_str());
    ok.lateMs = 80.49;
    CHECK(pn::SpeakerDelayFromRun(ok, Steady(200000)).delayMs == 280, "280.49 rounds down");
    ok.lateMs = 200;
    m = pn::SpeakerDelayFromRun(ok, Steady(900000));
    CHECK(!m.ok && Has(m.message, L"1100 ms") && Has(m.message, L"1 to 1000 ms"), "above the largest setting: %ls", m.message.c_str());
    CHECK(pn::SpeakerDelayFromRun(ok, Steady(900000), 1200).delayMs == 1100, "the limit is the caller's");
    ok.lateMs = -100;
    m = pn::SpeakerDelayFromRun(ok, Steady(25000));
    CHECK(!m.ok && Has(m.message, L"1 to 1000 ms"), "a negative delay: %ls", m.message.c_str());
    ok.lateMs = 80;

    /* the driver's value changed during the run: the speaker reported a new delay, or a setting was applied */
    pn::AppliedDelayReadings moved;
    moved.Add(200000, 6, false); moved.Add(200000, 6, false); moved.Add(364000, 6, false); moved.Add(364000, 6, true);
    m = pn::SpeakerDelayFromRun(ok, moved);
    CHECK(!m.ok && Has(m.message, L"changed its delay") && Has(m.message, L"200 ms") && Has(m.message, L"364 ms"), "changed: %ls", m.message.c_str());
    pn::AppliedDelayReadings back;
    back.Add(200000, 6, false); back.Add(364000, 6, false); back.Add(200000, 6, true);
    CHECK(!pn::SpeakerDelayFromRun(ok, back).ok, "changed and back is still a change");
    pn::AppliedDelayReadings codec = Steady(200000);
    codec.Add(200000, 1, true);
    m = pn::SpeakerDelayFromRun(ok, codec);
    CHECK(!m.ok && Has(m.message, L"codec"), "the codec changed: %ls", m.message.c_str());
    CHECK(Steady(200000, 6).codec == 6 && !Steady(200000, 6).codecChanged, "the codec of the run");

    /* the driver's value is missing */
    m = pn::SpeakerDelayFromRun(ok, pn::AppliedDelayReadings());
    CHECK(!m.ok && Has(m.message, L"0.1.0.25"), "no readings: %ls", m.message.c_str());
    m = pn::SpeakerDelayFromRun(ok, Steady(0));
    CHECK(!m.ok && Has(m.message, L"0.1.0.25"), "a driver that reports 0 (before 0.1.0.25): %ls", m.message.c_str());
    pn::AppliedDelayReadings early;
    early.Add(200000, 6, false); early.Add(200000, 6, false);
    m = pn::SpeakerDelayFromRun(ok, early);
    CHECK(!m.ok && Has(m.message, L"end of the test"), "no reading after the last chirp: %ls", m.message.c_str());
    /* a reading without a stream (0) next to real ones: the stream started late or restarted
     * during the test on a driver that reports the value, not an old driver. The reading
     * without a stream may carry no codec: still the restart, not a codec change. */
    pn::AppliedDelayReadings started;
    started.Add(0, 0, false);
    for (int k = 0; k < 8; ++k) started.Add(200000, 6, false);
    started.Add(200000, 6, true);
    m = pn::SpeakerDelayFromRun(ok, started);
    CHECK(!m.ok && Has(m.message, L"restarted") && !Has(m.message, L"0.1.0.25") && !Has(m.message, L"codec"), "the stream started during the test: %ls", m.message.c_str());
    pn::AppliedDelayReadings restarted;
    for (int k = 0; k < 5; ++k) restarted.Add(200000, 6, false);
    restarted.Add(0, 6, false);
    for (int k = 0; k < 3; ++k) restarted.Add(200000, 6, false);
    restarted.Add(200000, 6, true);
    m = pn::SpeakerDelayFromRun(ok, restarted);
    CHECK(!m.ok && Has(m.message, L"restarted") && !Has(m.message, L"0.1.0.25"), "the stream restarted during the test: %ls", m.message.c_str());

    /* a failed run keeps its own message */
    s = Scene(); s.gainPerChirp.assign(8, 0.0);
    r = Run(s);
    m = pn::SpeakerDelayFromRun(r, Steady(200000));
    CHECK(!m.ok && m.message == r.message && !m.message.empty(), "not heard: the analysis message");
    pn::LatencyResult cancelled;
    cancelled.verdict = pn::LatencyVerdict::Cancelled; cancelled.message = L"The measurement was cancelled.";
    CHECK(pn::SpeakerDelayFromRun(cancelled, Steady(200000)).message == cancelled.message, "cancelled");

    /* which progress steps the driver is read on */
    pn::LatencyProgress p;
    p.step = true;
    CHECK(!pn::AppliedDelayReadings::ReadOn(p), "not before the clock moves");
    p.phase = pn::LatencyPhase::LeadIn;
    CHECK(pn::AppliedDelayReadings::ReadOn(p), "lead-in step");
    p.phase = pn::LatencyPhase::Chirps; p.chirp = 4;
    CHECK(pn::AppliedDelayReadings::ReadOn(p), "chirp step");
    p.step = false;
    CHECK(!pn::AppliedDelayReadings::ReadOn(p), "not on the repeated calls");
    p.step = true; p.phase = pn::LatencyPhase::Finishing;
    CHECK(pn::AppliedDelayReadings::ReadOn(p) && pn::AppliedDelayReadings::Finishing(p), "finishing step");
    p.phase = pn::LatencyPhase::Analysing;
    CHECK(!pn::AppliedDelayReadings::ReadOn(p), "not after the streams stopped");
}

/* Read-only machine checks: no stream is opened. */
static void TestMachine()
{
    std::vector<pn::CaptureEndpoint> mics;
    HRESULT hr = pn::ListCaptureEndpoints(mics);
    unsigned defaults = 0; bool named = true;
    for (const auto &m : mics) { defaults += m.isDefault; named &= !m.id.empty() && !m.name.empty(); }
    printf("  microphones: hr=0x%08lx, %zu active, %u default\n", (unsigned long)hr, mics.size(), defaults);
    CHECK(SUCCEEDED(hr), "capture endpoints listed (0x%08lx)", (unsigned long)hr);
    CHECK(defaults <= 1 && named, "at most one default, every endpoint has an id and a name");

    pn::LatencyMeasurement m = pn::MeasureOutputLatency(L"", L"");
    CHECK(m.hr == E_INVALIDARG && m.result.verdict == pn::LatencyVerdict::Failed && !m.result.message.empty(), "no endpoint ids: nothing opened");
    int calls = 0;
    m = pn::MeasureOutputLatency(L"{0.0.0.00000000}.{00000000-0000-0000-0000-00000000dead}", L"{0.0.1.00000000}.{00000000-0000-0000-0000-00000000beef}",
                                 pn::LatencyOptions(), nullptr, [&](const pn::LatencyProgress &) { ++calls; });
    CHECK(FAILED(m.hr) && m.result.verdict == pn::LatencyVerdict::Failed && calls == 0, "unknown endpoints: refused before anything starts (0x%08lx, %d calls)",
          (unsigned long)m.hr, calls);
    CHECK(Has(m.result.message, L"not available"), "plain-language failure");
}

int main()
{
    TestSignal();
    TestKnownLag();
    TestNoiseAndLevels();
    TestJitter();
    TestAliases();
    TestLagRange();
    TestChannels();
    TestRejections();
    TestText();
    TestSpeakerDelay();
    TestMachine();
    printf(g_fail ? "FAILED (%d checks, %d failures)\n" : "PASSED (%d checks, %d failures)\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
