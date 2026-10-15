/* SPDX-License-Identifier: MIT */
#define NOMINMAX
#include "pnlatency.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <stdarg.h>
#include <stdio.h>
#include <wctype.h>
#include <algorithm>
#include <cmath>
#include <memory>

namespace pn {
using Microsoft::WRL::ComPtr;
namespace {
const double kPi = 3.14159265358979323846;
const UINT32 kChirpMs = 12;                  /* the chirp: 12 ms, 1 -> 7 kHz */
const double kChirpF0 = 1000, kChirpF1 = 7000;
const UINT32 kWideMs = 400;                  /* the "heard elsewhere" window, inside the smallest gap */
const UINT32 kMinCaptureRate = 16000;        /* below it the 7 kHz end of the chirp folds over */
const unsigned kMaxCaptureChannels = 8;      /* microphone channels kept and analysed */
/* FitLatencyLagRange: the default range reaches 500 ms early, enough for an applied delay up to
 * 480 ms; a moved range starts 30 ms (refineMs + toleranceMs) before -applied and keeps the
 * default 2000 ms span. */
const UINT32 kKeepLagRangeMs = 480, kMaxLagShiftMs = 1000;
const int kLagMarginMs = 30, kLagSpanMs = 2000;

std::wstring Format(const wchar_t *format, ...)
{
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    return buffer;
}

UINT32 ChirpLength(UINT32 rate) { return rate * kChirpMs / 1000; }

std::wstring LowRateMessage(UINT32 rate)
{
    return Format(L"This microphone records at %u Hz, too low for the test sound (Bluetooth headset microphones often record this way). "
                  L"Choose a microphone that records at 16 kHz or more.", rate);
}

/* 4 accumulators: no serial dependency on one sum, no /fp:fast needed */
double Dot(const float *a, const float *b, size_t n)
{
    double s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        s0 += (double)a[i] * b[i]; s1 += (double)a[i + 1] * b[i + 1];
        s2 += (double)a[i + 2] * b[i + 2]; s3 += (double)a[i + 3] * b[i + 3];
    }
    for (; i < n; ++i) s0 += (double)a[i] * b[i];
    return (s0 + s1) + (s2 + s3);
}

double Median(std::vector<double> v)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : (v[m - 1] + v[m]) / 2;
}

/* Microphone sample index <-> QPC time (ms after the first packet), from the packet stamps:
 * a sample is timed from the last stamped packet at or before it. */
class CaptureTime {
public:
    CaptureTime(const LatencyRecording &r, UINT64 base) : m_rate(r.captureRate)
    {
        for (const auto &p : r.packets)
            if (p.index < r.capture.size() && (m_byIndex.empty() || p.index > m_byIndex.back().index))
                m_byIndex.push_back({ (double)p.index, (double)(INT64)(p.qpc100ns - base) / 10000.0 });
        m_byTime = m_byIndex;
        std::stable_sort(m_byTime.begin(), m_byTime.end(), [](const Stamp &a, const Stamp &b) { return a.ms < b.ms; });
    }
    bool Empty() const { return m_byIndex.empty(); }
    double Ms(double index) const
    {
        auto it = std::upper_bound(m_byIndex.begin(), m_byIndex.end(), index, [](double v, const Stamp &s) { return v < s.index; });
        const Stamp &s = it == m_byIndex.begin() ? *it : *(it - 1);
        return s.ms + (index - s.index) * 1000.0 / m_rate;
    }
    /* approximate inverse; callers widen the range and filter with Ms() */
    double Index(double ms) const
    {
        auto it = std::upper_bound(m_byTime.begin(), m_byTime.end(), ms, [](double v, const Stamp &s) { return v < s.ms; });
        const Stamp &s = it == m_byTime.begin() ? *it : *(it - 1);
        return s.index + (ms - s.ms) * m_rate / 1000.0;
    }
private:
    struct Stamp { double index, ms; };
    std::vector<Stamp> m_byIndex, m_byTime;
    double m_rate;
};

/* One chirp's matched filter over its whole lag range: normalized correlation per microphone
 * position (-1 outside the range) and that position's lateness. */
struct Track {
    bool timed = false;
    double presentedMs = 0;
    size_t first = 0;               /* microphone index of nc[0] */
    std::vector<float> nc, lateMs;
};

/* best position within [lo, hi] ms of lateness; -1 when the range holds no position */
ptrdiff_t Best(const Track &t, double lo, double hi)
{
    ptrdiff_t best = -1;
    for (size_t i = 0; i < t.nc.size(); ++i)
        if (t.nc[i] >= 0 && t.lateMs[i] >= lo && t.lateMs[i] <= hi && (best < 0 || t.nc[i] > t.nc[(size_t)best])) best = (ptrdiff_t)i;
    return best;
}
} // namespace

std::vector<float> ChirpSignal(UINT32 rate)
{
    const UINT32 n = ChirpLength(rate);
    std::vector<float> c(n);
    const double seconds = kChirpMs / 1000.0;
    for (UINT32 i = 0; i < n; ++i) {
        double t = (double)i / rate, w = 0.5 - 0.5 * cos(2 * kPi * i / (n - 1));
        c[i] = (float)(w * sin(2 * kPi * (kChirpF0 * t + (kChirpF1 - kChirpF0) * t * t / (2 * seconds))));
    }
    return c;
}

std::vector<UINT64> LatencyChirpFrames(const LatencyOptions &o, UINT32 rate)
{
    std::vector<UINT64> frames;
    if (!rate) return frames;
    UINT64 ms = o.leadInMs;
    frames.push_back(ms * rate / 1000);
    for (UINT32 gap : o.spacingMs) { ms += gap; frames.push_back(ms * rate / 1000); }
    return frames;
}

UINT64 LatencyTotalFrames(const LatencyOptions &o, UINT32 rate)
{
    std::vector<UINT64> frames = LatencyChirpFrames(o, rate);
    if (frames.empty()) return 0;
    UINT64 tailMs = std::max<INT64>((INT64)o.tailMs, (INT64)std::max(o.minLagMs, o.maxLagMs) + 500);
    return frames.back() + ChirpLength(rate) + tailMs * rate / 1000;
}

void FitLatencyLagRange(LatencyOptions &o, UINT32 appliedMs)
{
    if (appliedMs <= kKeepLagRangeMs) return;
    o.minLagMs = -(int)std::min(appliedMs, kMaxLagShiftMs) - kLagMarginMs;
    o.maxLagMs = o.minLagMs + kLagSpanMs;
}

LatencyResult AnalyzeLatency(const LatencyRecording &r, const LatencyOptions &o)
{
    LatencyResult res;
    res.total = (unsigned)r.chirpFrames.size();
    res.chirps.resize(res.total);
    const std::vector<float> tpl = ChirpSignal(r.captureRate);
    const size_t n = tpl.size();
    if (!res.total) { res.verdict = LatencyVerdict::Failed; res.message = L"No chirps were scheduled."; return res; }
    if (r.captureRate && r.captureRate < kMinCaptureRate) {   /* the chirp would fold over and its upper half be filtered out */
        res.verdict = LatencyVerdict::Failed;
        res.message = LowRateMessage(r.captureRate);
        return res;
    }
    if (!r.captureRate || n < 8 || r.capture.size() < n || r.packets.empty()) {
        res.verdict = LatencyVerdict::NoRecording;
        res.message = L"The microphone delivered no timed recording. Check that it is connected and enabled, then try again.";
        return res;
    }
    const UINT64 base = r.packets.front().qpc100ns;
    CaptureTime capture(r, base);
    if (capture.Empty()) {
        res.verdict = LatencyVerdict::NoRecording;
        res.message = L"The microphone delivered no timed recording. Check that it is connected and enabled, then try again.";
        return res;
    }

    /* presentation time of each chirp's first frame, from the clock sample nearest in frames;
     * samples before the position first moved carry no timing */
    std::vector<LatencyRecording::ClockPoint> clock;
    for (const auto &p : r.clock) if (p.frame > 0 && p.qpc100ns) clock.push_back(p);
    std::stable_sort(clock.begin(), clock.end(), [](const auto &a, const auto &b) { return a.frame < b.frame; });
    std::vector<Track> tracks(res.total);
    unsigned timed = 0;
    for (unsigned k = 0; k < res.total && r.renderRate && !clock.empty(); ++k) {
        const double f = (double)r.chirpFrames[k];
        auto it = std::lower_bound(clock.begin(), clock.end(), f, [](const auto &p, double v) { return p.frame < v; });
        const LatencyRecording::ClockPoint *p = nullptr;
        if (it != clock.end()) p = &*it;
        if (it != clock.begin() && (!p || f - (it - 1)->frame <= p->frame - f)) {
            /* the earliest sample of an equal-frame run before the chirp */
            auto prev = it - 1;
            while (prev != clock.begin() && (prev - 1)->frame == prev->frame) --prev;
            p = &*prev;
        }
        if (!p || fabs(p->frame - f) > r.renderRate) continue;   /* no clock sample within 1 s */
        tracks[k].timed = res.chirps[k].timed = true;
        tracks[k].presentedMs = res.chirps[k].presentedMs =
            (double)(INT64)(p->qpc100ns - base) / 10000.0 + (f - p->frame) * 1000.0 / r.renderRate;
        ++timed;
    }
    const unsigned minClear = std::max(1u, std::min(o.minClear, res.total));
    if (timed < minClear) {
        res.verdict = LatencyVerdict::NoClock;
        res.message = !timed ? std::wstring(L"The output's audio clock did not report playback of the test sound. Check that the speaker is connected and playing, then try again.")
                             : Format(L"The output's audio clock stopped during the test (%u of %u chirps timed). Check that the speaker stays connected, then try again.", timed, res.total);
        return res;
    }

    /* matched filter: normalized correlation of the chirp at each microphone position */
    const std::vector<float> &x = r.capture;
    const size_t positions = x.size() - n + 1;
    std::vector<double> energy(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); ++i) energy[i + 1] = energy[i] + (double)x[i] * x[i];
    double tplEnergy = 0;
    for (float v : tpl) tplEnergy += (double)v * v;
    const int minLag = std::min(o.minLagMs, o.maxLagMs), maxLag = std::max(o.minLagMs, o.maxLagMs);
    const int tol = (int)o.toleranceMs;
    const double refine = o.refineMs;
    const double lateLo = minLag - refine - tol, lateHi = maxLag + refine + tol;
    const double margin = 0.05 * r.captureRate;   /* 50 ms of samples around the approximate range */
    for (auto &t : tracks) {
        if (!t.timed) continue;
        double a = capture.Index(t.presentedMs + lateLo) - margin, b = capture.Index(t.presentedMs + lateHi) + margin;
        size_t i0 = (size_t)std::max(0.0, a), i1 = (size_t)std::min((double)positions, std::max(0.0, b));
        if (i1 <= i0) continue;
        t.first = i0;
        t.nc.assign(i1 - i0, -1.0f);
        t.lateMs.assign(i1 - i0, 0.0f);
        for (size_t i = i0; i < i1; ++i) {
            double late = capture.Ms((double)i) - t.presentedMs;
            if (late < lateLo || late > lateHi) continue;
            double e = energy[i + n] - energy[i], c = 0;
            if (e > 1e-12) c = std::min(1.0, fabs(Dot(&x[i], tpl.data(), n)) / sqrt(e * tplEnergy));
            t.nc[i - i0] = (float)c;
            t.lateMs[i - i0] = (float)late;
        }
    }

    /* one lag for the whole run: per chirp the best match in 1 ms lateness bins, widened by
     * the tolerance, summed over the chirps; a wrong lag lines up at most a neighbour or two */
    const int lagBins = maxLag - minLag + 1, lateBins = lagBins + 2 * tol;
    std::vector<double> score(lagBins, 0.0);
    std::vector<float> binned(lateBins);
    for (const auto &t : tracks) {
        if (t.nc.empty()) continue;
        std::fill(binned.begin(), binned.end(), 0.0f);
        for (size_t i = 0; i < t.nc.size(); ++i) {
            if (t.nc[i] < 0) continue;
            int bin = (int)floor(t.lateMs[i] - (minLag - tol) + 0.5);
            if (bin >= 0 && bin < lateBins) binned[bin] = std::max(binned[bin], t.nc[i]);
        }
        for (int b = 0; b < lagBins; ++b) {
            float env = 0;
            for (int j = b; j <= b + 2 * tol; ++j) env = std::max(env, binned[j]);
            score[b] += env;
        }
    }
    const int bestBin = (int)(std::max_element(score.begin(), score.end()) - score.begin());
    double lag = minLag + bestBin;
    /* centre the refinement on the clear chirps near that lag: the score stays flat while
     * every chirp is inside the tolerance, and the first maximum is that plateau's early edge.
     * +-0.5 ms: the 1 ms bins the score used. */
    std::vector<double> nearLag;
    for (const auto &t : tracks) {
        ptrdiff_t i = t.nc.empty() ? -1 : Best(t, lag - tol - 0.5, lag + tol + 0.5);
        if (i >= 0 && t.nc[(size_t)i] >= o.clearMatch) nearLag.push_back(t.lateMs[(size_t)i]);
    }
    if (!nearLag.empty()) lag = Median(nearLag);
    res.lagMs = lag;

    UINT32 minGap = kWideMs + 100;
    for (UINT32 g : o.spacingMs) minGap = std::min(minGap, g);
    const double wide = std::max(refine, (double)std::min<UINT32>(kWideMs, minGap > 100 ? minGap - 100 : 0));
    std::vector<double> clearLate, elsewhere;   /* elsewhere: heard clearly, but away from the run's lag */
    double heardMin = 0, heardMax = 0;
    for (unsigned k = 0; k < res.total; ++k) {
        const Track &t = tracks[k];
        LatencyChirpResult &c = res.chirps[k];
        if (t.nc.empty()) continue;
        ptrdiff_t i = Best(t, lag - refine, lag + refine);
        if (i >= 0) {
            c.match = t.nc[(size_t)i];
            c.lateMs = capture.Ms((double)(t.first + (size_t)i)) - t.presentedMs;
            c.clear = c.match >= o.clearMatch;
        }
        ptrdiff_t w = Best(t, lag - wide, lag + wide);
        if (w >= 0) {
            c.heardMatch = t.nc[(size_t)w];
            c.heardLateMs = capture.Ms((double)(t.first + (size_t)w)) - t.presentedMs;
            c.heard = c.heardMatch >= o.clearMatch;
        }
        if (c.clear) { ++res.clear; clearLate.push_back(c.lateMs); }
        else if (c.heard && fabs(c.heardLateMs - lag) > refine) elsewhere.push_back(c.heardLateMs);
        if (c.heard || c.clear) {
            double v = c.clear ? c.lateMs : c.heardLateMs;
            heardMin = res.heard ? std::min(heardMin, v) : v;
            heardMax = res.heard ? std::max(heardMax, v) : v;
            ++res.heard;
        }
    }
    if (!clearLate.empty()) {
        res.lateMs = Median(clearLate);
        res.spreadMs = *std::max_element(clearLate.begin(), clearLate.end()) - *std::min_element(clearLate.begin(), clearLate.end());
    }
    /* One chirp heard clearly at another delay is a stray sound. Two or more that agree with
     * each other mean the speaker changed its delay during the run, even when six chirps
     * agree at the lag: in Fixed mode the driver's applied value does not move, so nothing
     * else catches it. */
    bool movedTogether = false;
    for (size_t i = 0; i < elsewhere.size() && !movedTogether; ++i)
        for (size_t j = i + 1; j < elsewhere.size() && !movedTogether; ++j) movedTogether = fabs(elsewhere[i] - elsewhere[j]) <= 2 * refine;

    if (res.clear >= minClear && res.spreadMs <= o.maxSpreadMs && !movedTogether) {
        res.verdict = LatencyVerdict::Ok;
        res.message = Format(L"%u of %u chirps heard clearly, spread %.1f ms: the sound comes %.1f ms %s than the output's clock says.",
                             res.clear, res.total, res.spreadMs, fabs(res.lateMs), res.lateMs < 0 ? L"earlier" : L"later");
    } else if (res.heard < minClear) {
        res.verdict = LatencyVerdict::NotHeard;
        if (energy.back() == 0)
            res.message = L"The microphone recorded only silence. Check that it is not muted and that desktop apps may use it "
                          L"(Windows Settings > Privacy & security > Microphone), then try again.";
        else if (!res.heard)
            res.message = L"No chirps were heard. Raise the speaker volume or move the microphone closer to the speaker (10 to 30 cm), then try again.";
        else
            res.message = Format(L"Only %u of %u chirps were heard clearly. Raise the speaker volume or move the microphone closer to the speaker "
                                 L"(10 to 30 cm), then try again.", res.heard, res.total);
    } else {
        res.verdict = LatencyVerdict::Inconsistent;
        res.message = Format(L"The chirps arrived with delays that differ by %.0f ms. Background noise, or the speaker changed its delay during "
                             L"the test. Try again in a quiet room with no other sound playing.", heardMax - heardMin);
    }
    return res;
}

LatencyResult AnalyzeLatencyChannels(const LatencyRecording &recording, const std::vector<std::vector<float>> &channels,
                                     const LatencyOptions &o, unsigned *channel)
{
    if (channel) *channel = 0;
    if (channels.empty()) return AnalyzeLatency(recording, o);
    LatencyRecording one;   /* everything but the samples */
    one.renderRate = recording.renderRate; one.chirpFrames = recording.chirpFrames; one.clock = recording.clock;
    one.captureRate = recording.captureRate; one.packets = recording.packets;
    LatencyResult best;
    double bestMatch = 0;
    bool have = false;
    for (size_t c = 0; c < channels.size(); ++c) {
        bool repeat = false;   /* a mono microphone on both channels: analysed once */
        for (size_t e = 0; e < c && !repeat; ++e) repeat = channels[e] == channels[c];
        if (repeat) continue;
        one.capture = channels[c];
        LatencyResult r = AnalyzeLatency(one, o);
        double match = 0;
        for (const auto &k : r.chirps) match += k.match;
        const bool better = !have || (r.ok() != best.ok() ? r.ok() : r.clear != best.clear ? r.clear > best.clear : match > bestMatch);
        if (!better) continue;
        best = std::move(r); bestMatch = match; have = true;
        if (channel) *channel = (unsigned)c;
    }
    return best;
}

std::wstring LatencyReport(const LatencyResult &r)
{
    std::wstring out = L"chirp   late by      match\n";
    for (size_t k = 0; k < r.chirps.size(); ++k) {
        const LatencyChirpResult &c = r.chirps[k];
        if (!c.timed) out += Format(L"%3zu     no clock time\n", k + 1);
        else if (c.clear) out += Format(L"%3zu     %+8.1f ms   %.2f\n", k + 1, c.lateMs, c.match);
        else if (c.heard) out += Format(L"%3zu     not at the run's delay: heard at %+.1f ms (%.2f)\n", k + 1, c.heardLateMs, c.heardMatch);
        else out += Format(L"%3zu     not heard     %.2f\n", k + 1, c.match);
    }
    if (r.clear) out += Format(L"median %+.1f ms, spread %.1f ms, %u of %u clear\n", r.lateMs, r.spreadMs, r.clear, r.total);
    return out + r.message + L"\n";
}

void AppliedDelayReadings::Add(UINT32 us, ULONG streamCodec, bool afterLastChirp)
{
    if (!count) { firstUs = minUs = maxUs = us; codec = streamCodec; }
    else {
        minUs = std::min(minUs, us); maxUs = std::max(maxUs, us);
        codecChanged |= streamCodec != codec;
    }
    lastUs = us;
    finishing |= afterLastChirp;
    ++count;
}

bool AppliedDelayReadings::ReadOn(const LatencyProgress &p)
{
    return p.step && (p.phase == LatencyPhase::LeadIn || p.phase == LatencyPhase::Chirps || p.phase == LatencyPhase::Finishing);
}

bool AppliedDelayReadings::Finishing(const LatencyProgress &p) { return p.step && p.phase == LatencyPhase::Finishing; }

SpeakerDelayMeasurement SpeakerDelayFromRun(const LatencyResult &r, const AppliedDelayReadings &a, UINT32 maxMs)
{
    SpeakerDelayMeasurement m;
    auto ms = [](UINT32 us) { return us % 1000 ? Format(L"%u.%u ms", us / 1000, us / 100 % 10) : Format(L"%u ms", us / 1000); };
    if (!r.ok()) { m.message = r.message; return m; }
    if (!a.count || !a.maxUs) {
        m.message = L"The driver did not report the speaker delay it applies, so the result cannot be turned into a setting. "
                    L"Driver 0.1.0.25 or later is needed.";
        return m;
    }
    /* 0 next to real readings: no stream at that moment (the reading may carry no codec, so
     * this comes before the codec check) */
    if (!a.minUs) { m.message = L"The speaker's Bluetooth stream started or restarted during the test. Wait a few seconds, then try again."; return m; }
    if (a.codecChanged) { m.message = L"The codec changed during the test. Wait until the stream has settled, then try again."; return m; }
    if (a.minUs != a.maxUs) {
        UINT32 other = a.lastUs != a.firstUs ? a.lastUs : a.maxUs != a.firstUs ? a.maxUs : a.minUs;
        m.message = L"The speaker changed its delay during the test (the driver applied " + ms(a.firstUs) + L", then " + ms(other) +
                    L"). Play nothing else to it, wait 10 seconds, then try again.";
        return m;
    }
    if (!a.finishing) { m.message = L"The driver's speaker delay could not be read at the end of the test. Try again."; return m; }
    m.appliedMs = a.firstUs / 1000.0;
    const double delay = floor(m.appliedMs + r.lateMs + 0.5);
    if (delay < 1 || delay > maxMs) {
        m.message = Format(L"The measured delay, %.0f ms, is outside 1 to %u ms. Check that the microphone hears this speaker and "
                           L"nothing else, then try again.", delay, maxMs);
        return m;
    }
    m.ok = true;
    m.delayMs = (UINT32)delay;
    m.message = Format(L"Measured %u ms (%u of %u chirps, spread %u ms).", m.delayMs, r.clear, r.total, (unsigned)floor(r.spreadMs + 0.5));
    return m;
}

int MatchCaptureEndpoint(const std::vector<CaptureEndpoint> &endpoints, const std::wstring &part)
{
    if (endpoints.empty()) return -1;
    if (part.empty()) {
        for (size_t i = 0; i < endpoints.size(); ++i) if (endpoints[i].isDefault) return (int)i;
        return 0;
    }
    for (size_t i = 0; i < endpoints.size(); ++i) if (!_wcsicmp(endpoints[i].name.c_str(), part.c_str())) return (int)i;
    auto lower = [](std::wstring s) { for (auto &ch : s) ch = (wchar_t)towlower(ch); return s; };
    const std::wstring needle = lower(part);
    int found = -1;
    for (size_t i = 0; i < endpoints.size(); ++i) {
        if (lower(endpoints[i].name).find(needle) == std::wstring::npos) continue;
        if (found >= 0) return -2;
        found = (int)i;
    }
    return found;
}

std::wstring LatencyProgressText(const LatencyProgress &p)
{
    switch (p.phase) {
    case LatencyPhase::Starting: return L"Starting the speaker...";
    case LatencyPhase::LeadIn: return L"Waiting for the speaker to start playing...";
    case LatencyPhase::Chirps: return Format(L"Chirp %u of %u...", p.chirp, p.chirps);
    case LatencyPhase::Finishing: return L"Listening for the last chirp...";
    case LatencyPhase::Analysing: return L"Analysing the recording...";
    }
    return L"";
}

/* ---- WASAPI ------------------------------------------------------------------ */
namespace {
struct ComScope {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(hr)) CoUninitialize(); }
};
struct CoMem { void operator()(void *p) const { CoTaskMemFree(p); } };

std::wstring FriendlyName(IMMDevice *device)
{
    ComPtr<IPropertyStore> store;
    PROPVARIANT v; PropVariantInit(&v);
    std::wstring name;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &v)) &&
        v.vt == VT_LPWSTR && v.pwszVal) name = v.pwszVal;
    PropVariantClear(&v);
    return name;
}

std::wstring DeviceId(IMMDevice *device)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::unique_ptr<wchar_t, CoMem> owned(id);
    return id;
}

/* the device for an id, only when it is active */
HRESULT ActiveDevice(IMMDeviceEnumerator *enumerator, const std::wstring &id, ComPtr<IMMDevice> &device)
{
    HRESULT hr = enumerator->GetDevice(id.c_str(), &device);
    if (FAILED(hr)) return hr;
    DWORD state = 0;
    if (FAILED(hr = device->GetState(&state))) return hr;
    return state == DEVICE_STATE_ACTIVE ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_READY);
}

enum class SampleKind { Unsupported, Float32, Int16, Int24, Int32 };
SampleKind KindOf(const WAVEFORMATEX *f)
{
    bool isFloat = f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, isPcm = f->wFormatTag == WAVE_FORMAT_PCM;
    if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE && f->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto *x = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
        isFloat = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
        isPcm = IsEqualGUID(x->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
    }
    if (!f->nChannels || f->nBlockAlign < f->nChannels * (f->wBitsPerSample / 8)) return SampleKind::Unsupported;
    if (isFloat && f->wBitsPerSample == 32) return SampleKind::Float32;
    if (isPcm && f->wBitsPerSample == 16) return SampleKind::Int16;
    if (isPcm && f->wBitsPerSample == 24) return SampleKind::Int24;
    if (isPcm && f->wBitsPerSample == 32) return SampleKind::Int32;   /* also 24 valid bits in 32 */
    return SampleKind::Unsupported;
}

/* one sample, full scale 1.0 */
float SampleAt(const BYTE *sample, SampleKind kind)
{
    switch (kind) {
    case SampleKind::Float32: { float v; memcpy(&v, sample, 4); return v; }
    case SampleKind::Int16: { INT16 v; memcpy(&v, sample, 2); return v / 32768.0f; }
    case SampleKind::Int24: { INT32 v = (INT32)((UINT32)sample[0] << 8 | (UINT32)sample[1] << 16 | (UINT32)sample[2] << 24); return v / 2147483648.0f; }
    case SampleKind::Int32: { INT32 v; memcpy(&v, sample, 4); return v / 2147483648.0f; }
    default: return 0;
    }
}

std::wstring Code(HRESULT hr) { return Format(L" (0x%08lx)", (unsigned long)hr); }
} // namespace

HRESULT ListCaptureEndpoints(std::vector<CaptureEndpoint> &endpoints)
{
    endpoints.clear();
    ComScope com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE) return com.hr;
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return hr;
    std::wstring defaultId;
    ComPtr<IMMDevice> device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &device))) defaultId = DeviceId(device.Get());
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return hr;
    UINT count = 0;
    if (FAILED(hr = devices->GetCount(&count))) return hr;
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> item;
        if (FAILED(devices->Item(i, &item))) continue;
        CaptureEndpoint e;
        e.id = DeviceId(item.Get());
        if (e.id.empty()) continue;
        e.name = FriendlyName(item.Get());
        if (e.name.empty()) e.name = e.id;
        e.isDefault = !defaultId.empty() && !_wcsicmp(e.id.c_str(), defaultId.c_str());
        endpoints.push_back(std::move(e));
    }
    return S_OK;
}

LatencyMeasurement MeasureOutputLatency(const std::wstring &renderId, const std::wstring &captureId, const LatencyOptions &options,
                                        const std::atomic<bool> *cancel, const LatencyProgressFn &progress)
{
    LatencyMeasurement m;
    const ULONGLONG started = GetTickCount64();
    auto finish = [&](HRESULT hr, LatencyVerdict verdict, std::wstring text) {
        m.hr = hr;
        m.result.verdict = verdict;
        m.result.message = std::move(text);
        m.elapsedMs = GetTickCount64() - started;
        return m;
    };
    if (renderId.empty() || captureId.empty()) return finish(E_INVALIDARG, LatencyVerdict::Failed, L"Select the speaker and a microphone first.");
    ComScope com;
    if (FAILED(com.hr) && com.hr != RPC_E_CHANGED_MODE)
        return finish(com.hr, LatencyVerdict::Failed, L"Windows audio could not be initialized." + Code(com.hr));
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) return finish(hr, LatencyVerdict::Failed, L"Windows audio could not be reached." + Code(hr));
    /* both endpoints are resolved before anything is opened */
    ComPtr<IMMDevice> renderDevice, captureDevice;
    if (FAILED(hr = ActiveDevice(enumerator.Get(), renderId, renderDevice)))
        return finish(hr, LatencyVerdict::Failed, L"The speaker's Windows audio output is not available. Connect the speaker and try again." + Code(hr));
    if (FAILED(hr = ActiveDevice(enumerator.Get(), captureId, captureDevice)))
        return finish(hr, LatencyVerdict::Failed, L"The selected microphone is not available. Connect it or choose another one." + Code(hr));
    m.renderName = FriendlyName(renderDevice.Get());
    m.captureName = FriendlyName(captureDevice.Get());

    /* the output: shared mode in its float mix format */
    ComPtr<IAudioClient> render;
    if (FAILED(hr = renderDevice->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void **>(render.GetAddressOf()))))
        return finish(hr, LatencyVerdict::Failed, L"The speaker's Windows audio output could not be opened." + Code(hr));
    WAVEFORMATEX *rawFormat = nullptr;
    if (FAILED(hr = render->GetMixFormat(&rawFormat)))
        return finish(hr, LatencyVerdict::Failed, L"The speaker's Windows audio format could not be read." + Code(hr));
    std::unique_ptr<WAVEFORMATEX, CoMem> renderFormat(rawFormat);
    if (KindOf(renderFormat.get()) != SampleKind::Float32 || !renderFormat->nSamplesPerSec)
        return finish(AUDCLNT_E_UNSUPPORTED_FORMAT, LatencyVerdict::Failed,
                      L"The speaker's Windows mix format is not 32-bit float, so the test sound cannot be played.");
    if (FAILED(hr = render->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, 3000000, 0, renderFormat.get(), nullptr)))
        return finish(hr, LatencyVerdict::Failed, L"The speaker's Windows audio output could not be started. Close apps that use it exclusively and try again." + Code(hr));

    /* the microphone: shared mode, raw (no effects in the timing path) where the endpoint allows it */
    ComPtr<IAudioClient> capture;
    std::unique_ptr<WAVEFORMATEX, CoMem> captureFormat;
    for (int attempt = 0; attempt < 2 && !captureFormat; ++attempt) {
        capture.Reset();
        if (FAILED(hr = captureDevice->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr, reinterpret_cast<void **>(capture.GetAddressOf()))))
            return finish(hr, LatencyVerdict::Failed, L"The microphone could not be opened." + Code(hr));
        m.captureRaw = false;
        ComPtr<IAudioClient2> capture2;
        if (attempt == 0 && SUCCEEDED(capture.As(&capture2))) {
            AudioClientProperties properties = {};
            properties.cbSize = sizeof(properties);
            properties.eCategory = AudioCategory_Other;
            properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
            m.captureRaw = SUCCEEDED(capture2->SetClientProperties(&properties));
        }
        if (attempt == 0 && !m.captureRaw) attempt = 1;   /* no raw mode: this is the plain attempt */
        WAVEFORMATEX *format = nullptr;
        if (FAILED(hr = capture->GetMixFormat(&format)))
            return finish(hr, LatencyVerdict::Failed, L"The microphone's Windows audio format could not be read." + Code(hr));
        std::unique_ptr<WAVEFORMATEX, CoMem> owned(format);
        if (KindOf(owned.get()) == SampleKind::Unsupported || !owned->nSamplesPerSec)
            return finish(AUDCLNT_E_UNSUPPORTED_FORMAT, LatencyVerdict::Failed, L"The microphone's Windows audio format is not supported (it must be PCM or float).");
        if (owned->nSamplesPerSec < kMinCaptureRate) {   /* before anything is started: nothing plays */
            m.captureRate = owned->nSamplesPerSec;
            return finish(AUDCLNT_E_UNSUPPORTED_FORMAT, LatencyVerdict::Failed, LowRateMessage(owned->nSamplesPerSec));
        }
        hr = capture->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_NOPERSIST, 10000000, 0, owned.get(), nullptr);
        if (SUCCEEDED(hr)) captureFormat = std::move(owned);
        else if (attempt == 1) return finish(hr, LatencyVerdict::Failed, L"The microphone could not be started. Close apps that use it exclusively and try again." + Code(hr));
    }
    const SampleKind captureKind = KindOf(captureFormat.get());
    m.renderRate = renderFormat->nSamplesPerSec;
    m.captureRate = captureFormat->nSamplesPerSec;
    m.captureChannels = captureFormat->nChannels;
    m.captureBits = captureFormat->wBitsPerSample;
    m.captureFloat = captureKind == SampleKind::Float32;

    ComPtr<IAudioRenderClient> renderClient;
    ComPtr<IAudioCaptureClient> captureClient;
    ComPtr<IAudioClock> clock;
    UINT64 frequency = 0;
    UINT32 bufferFrames = 0;
    if (FAILED(hr = render->GetService(IID_PPV_ARGS(&renderClient))) || FAILED(hr = capture->GetService(IID_PPV_ARGS(&captureClient))) ||
        FAILED(hr = render->GetService(IID_PPV_ARGS(&clock))) || FAILED(hr = clock->GetFrequency(&frequency)) ||
        FAILED(hr = render->GetBufferSize(&bufferFrames)))
        return finish(hr, LatencyVerdict::Failed, L"Windows audio did not provide the streams' services." + Code(hr));
    if (!frequency) return finish(E_UNEXPECTED, LatencyVerdict::Failed, L"The speaker's audio clock has no frequency.");

    const UINT32 rate = m.renderRate, channels = renderFormat->nChannels;
    const std::vector<float> chirp = ChirpSignal(rate);
    const std::vector<UINT64> at = LatencyChirpFrames(options, rate);
    const UINT64 total = LatencyTotalFrames(options, rate);
    const float amplitude = std::min(1.0f, std::max(0.0f, options.amplitude));
    const UINT32 ahead = std::min(bufferFrames, rate / 10);   /* keep 100 ms queued */
    LatencyRecording rec;
    rec.renderRate = rate;
    rec.captureRate = m.captureRate;
    rec.chirpFrames = at;
    /* every microphone channel (the first 8): the microphone may be on an interface's second
     * input; AnalyzeLatencyChannels picks the one that heard the chirps */
    const unsigned keep = std::min<unsigned>(captureFormat->nChannels, kMaxCaptureChannels);
    const UINT32 sampleBytes = captureFormat->wBitsPerSample / 8;
    std::vector<std::vector<float>> recorded(keep);
    for (auto &track : recorded) track.reserve((size_t)((total * 1000 / rate + 3000) * m.captureRate / 1000));
    UINT64 written = 0;
    auto fill = [&]() -> HRESULT {
        UINT32 padding = 0;
        HRESULT h = render->GetCurrentPadding(&padding);
        if (FAILED(h) || padding >= ahead) return h;
        const UINT32 frames = ahead - padding;
        BYTE *data = nullptr;
        if (FAILED(h = renderClient->GetBuffer(frames, &data))) return h;
        float *out = reinterpret_cast<float *>(data);
        for (UINT32 i = 0; i < frames; ++i) {
            const UINT64 frame = written + i;
            float v = 0;
            for (UINT64 start : at) if (frame >= start && frame < start + chirp.size()) v = amplitude * chirp[(size_t)(frame - start)];
            for (UINT32 c = 0; c < channels; ++c) out[(size_t)i * channels + c] = v;   /* silence after the run */
        }
        written += frames;
        return renderClient->ReleaseBuffer(frames, 0);
    };
    const size_t captureLimit = (size_t)((total * 1000 / rate + 30000) * m.captureRate / 1000);
    auto drain = [&]() -> HRESULT {
        for (;;) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devicePosition = 0, qpc = 0;
            HRESULT h = captureClient->GetBuffer(&data, &frames, &flags, &devicePosition, &qpc);
            if (h == AUDCLNT_S_BUFFER_EMPTY) return S_OK;
            if (FAILED(h)) return h;
            if (recorded[0].size() + frames <= captureLimit) {
                if (!(flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) && qpc) rec.packets.push_back({ recorded[0].size(), qpc });
                for (UINT32 i = 0; i < frames; ++i) {
                    const BYTE *frame = data + (size_t)i * captureFormat->nBlockAlign;
                    for (unsigned c = 0; c < keep; ++c)
                        recorded[c].push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : SampleAt(frame + (size_t)c * sampleBytes, captureKind));
                }
            }
            if (FAILED(h = captureClient->ReleaseBuffer(frames))) return h;
        }
    };

    LatencyProgress p;
    p.chirps = (unsigned)at.size();
    auto report = [&](LatencyPhase phase, unsigned chirp, unsigned percent, bool step) {
        if (!progress) return;
        p.phase = phase; p.chirp = chirp; p.percent = percent; p.step = step;
        progress(p);
    };
    report(LatencyPhase::Starting, 0, 0, true);
    if (FAILED(hr = fill()) || FAILED(hr = capture->Start()))
        return finish(hr, LatencyVerdict::Failed, L"The measurement streams could not be started." + Code(hr));
    if (FAILED(hr = render->Start())) {
        capture->Stop();
        return finish(hr, LatencyVerdict::Failed, L"The speaker's Windows audio output could not be started." + Code(hr));
    }
    const ULONGLONG deadline = GetTickCount64() + total * 1000 / rate + 20000;
    ULONGLONG lastMove = GetTickCount64(), nextReport = lastMove + 250;
    double presented = 0;
    LatencyPhase phase = LatencyPhase::Starting;
    unsigned chirpsPresented = 0;
    LatencyVerdict stopped = LatencyVerdict::Ok;
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) { stopped = LatencyVerdict::Cancelled; break; }
        if (FAILED(hr = fill())) break;
        UINT64 position = 0, qpc = 0;
        if (FAILED(hr = clock->GetPosition(&position, &qpc))) break;
        const double frame = (double)position * rate / frequency;
        if (qpc && frame > 0 && (rec.clock.empty() || rec.clock.back().frame != frame || rec.clock.back().qpc100ns != qpc))
            rec.clock.push_back({ frame, qpc });
        const ULONGLONG now = GetTickCount64();
        if (frame > presented) { presented = frame; lastMove = now; }
        if (FAILED(hr = drain())) break;
        unsigned count = 0;
        for (UINT64 start : at) count += presented >= (double)start;
        LatencyPhase next = presented <= 0 ? LatencyPhase::Starting : !count ? LatencyPhase::LeadIn
                          : presented >= (double)(at.back() + chirp.size()) ? LatencyPhase::Finishing : LatencyPhase::Chirps;
        const unsigned percent = (unsigned)std::min(95.0, presented * 95.0 / (double)total);
        if (next != phase || count != chirpsPresented) {
            phase = next; chirpsPresented = count;
            report(phase, count, percent, true);
            nextReport = now + 250;
        } else if (now >= nextReport) {
            report(phase, count, percent, false);
            nextReport = now + 250;
        }
        if (presented >= (double)total) break;   /* every frame of the run has been presented */
        if (now - lastMove > 8000 || now > deadline) { stopped = LatencyVerdict::NoClock; break; }
        Sleep(5);
    }
    render->Stop();
    HRESULT drained = drain();
    capture->Stop();
    if (SUCCEEDED(hr) && FAILED(drained)) hr = drained;
    if (stopped == LatencyVerdict::Cancelled) return finish(HRESULT_FROM_WIN32(ERROR_CANCELLED), LatencyVerdict::Cancelled, L"The measurement was cancelled.");
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED)
        return finish(hr, LatencyVerdict::Failed, L"The speaker or the microphone was disconnected during the test." + Code(hr));
    if (FAILED(hr)) return finish(hr, LatencyVerdict::Failed, L"Windows audio stopped the measurement." + Code(hr));
    if (stopped == LatencyVerdict::NoClock)
        return finish(HRESULT_FROM_WIN32(ERROR_TIMEOUT), LatencyVerdict::NoClock, presented <= 0
            ? std::wstring(L"The speaker's audio clock did not start. Check that the speaker is connected and not used exclusively by another app, then try again.")
            : std::wstring(L"The speaker's audio clock stopped during the test. Check that the speaker stays connected, then try again."));
    report(LatencyPhase::Analysing, chirpsPresented, 97, true);
    unsigned used = 0;
    m.result = AnalyzeLatencyChannels(rec, recorded, options, &used);
    m.captureChannel = (WORD)used;
    m.hr = S_OK;
    m.elapsedMs = GetTickCount64() - started;
    return m;
}

} // namespace pn
