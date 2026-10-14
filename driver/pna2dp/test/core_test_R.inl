/* Lane R: the sample-rate converter (0.1.0.22 #1: pcm.cpp, resample_coeffs.h). Included by
 * core_test.cpp; uses its CHECK / checks / failures. The double precision here is the test's own:
 * the converter is fixed point only. Every figure is measured on the shipped integer tables.
 * SPDX-License-Identifier: MIT */
#include <complex>
#include <chrono>
#include <algorithm>

static const double kRPi = 3.14159265358979323846;
static const double kRQ28 = 268435456.0;

/* Every bridged pair: decimation and its 2:1 FIR, polyphase table, latency the plan reports (engine
 * frames). A cascade runs the polyphase stage first, into the other family's double rate Mid, then
 * that rate's 2:1 FIR, which stops from the final Nyquist frequency on its own (24 kHz from 96 kHz,
 * 22.05 kHz from 88.2 kHz); the polyphase stage stops what would fold onto the final band at Mid. */
struct RPair { ULONG Engine, Codec, Decimate; const PN_DEC2_TABLE *Dec; const PN_RS_TABLE *Rs; ULONG Delay; };
static const RPair kRPairs[] = {
    { 96000, 48000, 2, &g_PnDec96, NULL, 97 }, { 88200, 44100, 2, &g_PnDec88, NULL, 173 },
    { 44100, 48000, 1, NULL, &g_PnRs44to48, 86 }, { 48000, 44100, 1, NULL, &g_PnRs48to44, 94 },
    { 88200, 48000, 2, &g_PnDec96, &g_PnRs88to96, 98 }, { 96000, 44100, 2, &g_PnDec88, &g_PnRs96to88, 199 },
};

/* the polyphase stage's output rate: the codec's, or in a cascade twice it */
static double RMid(const RPair &P) { return (double)P.Codec * P.Decimate; }

struct RRand {
    ULONG s;
    explicit RRand(ULONG Seed) : s(Seed ? Seed : 1) {}
    ULONG Next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    double Uni() { return Next() / 4294967296.0; }
    double Tpdf() { return Uni() + Uni() - 1.0; }
};

/* One stream through the converter: interleaved input in its own format, Target output. */
struct RConv {
    PN_PCM_RATE_PLAN Plan;
    PN_PCM_STATE St;
    BOOLEAN Ok;
    RConv(ULONG Engine, ULONG Codec, BOOLEAN FadeIn = FALSE) { Ok = PnPcmRatePlan(Engine, Codec, &Plan); PnPcmStateResetRate(&St, &Plan, FadeIn); }
    /* Chunk 0: one call; else blocks of 1..Chunk frames from Rnd (or exactly Chunk without Rnd) */
    std::vector<UCHAR> Run(const UCHAR *In, ULONG Frames, ULONG InCh, ULONG Container, ULONG Valid, BOOLEAN IsFloat, ULONG Target, ULONG OutCh,
                           ULONG Gain = 32768, ULONG Chunk = 0, RRand *Rnd = NULL)
    {
        ULONG inFrameBytes = InCh * Container / 8;
        ULONG outBps = Target == PNA2DP_FMT_S16 ? 2 : Target == PNA2DP_FMT_S24 ? 3 : 4;
        std::vector<UCHAR> out((size_t)(Frames / 2 + 64) * 3 * OutCh * outBps);
        ULONG done = 0, written = 0;
        while (done < Frames) {
            ULONG n = Frames - done;
            if (Chunk) {
                ULONG c = Rnd ? 1 + Rnd->Next() % Chunk : Chunk;   /* drawn once (PN_MIN evaluates twice) */
                if (c < n) n = c;
            }
            written += PnConvertPcmEx(&St, In + (size_t)done * inFrameBytes, n * inFrameBytes, InCh, Container, Valid, IsFloat,
                                      out.data() + written, (ULONG)out.size() - written, Target, OutCh, Gain);
            done += n;
        }
        out.resize(written);
        return out;
    }
};

static std::vector<double> RDecode(const std::vector<UCHAR> &B, ULONG Target)
{
    std::vector<double> x;
    if (Target == PNA2DP_FMT_S16) { for (size_t i = 0; i + 2 <= B.size(); i += 2) { SHORT v; memcpy(&v, &B[i], 2); x.push_back(v / 32768.0); } }
    else if (Target == PNA2DP_FMT_S24) {
        for (size_t i = 0; i + 3 <= B.size(); i += 3) x.push_back((double)(LONG)(((ULONG)B[i] << 8) | ((ULONG)B[i + 1] << 16) | ((ULONG)B[i + 2] << 24)) / 2147483648.0);
    } else if (Target == PNA2DP_FMT_F32) { for (size_t i = 0; i + 4 <= B.size(); i += 4) { float v; memcpy(&v, &B[i], 4); x.push_back(v); } }
    else { for (size_t i = 0; i + 4 <= B.size(); i += 4) { LONG v; memcpy(&v, &B[i], 4); x.push_back(v / 2147483648.0); } }
    return x;
}

/* Least squares: DC plus a sinusoid at each of Freqs (Hz) in channel Ch of Chs interleaved
 * channels, samples [Begin, End) at rate Fs. Amplitudes and phases (cosine phase at n = 0). */
struct RFit { std::vector<double> Amp, Phase; double Resid; };
static RFit RFitTones(const std::vector<double> &X, ULONG Chs, ULONG Ch, size_t Begin, size_t End, double Fs, const std::vector<double> &Freqs)
{
    const size_t P = 2 * Freqs.size() + 1;
    RFit fit;
    End = PN_MIN(End, X.size() / Chs);
    if (End <= Begin + P) {   /* too little output (a broken converter): a fit that fails every check */
        fit.Amp.assign(Freqs.size(), 0.0); fit.Phase.assign(Freqs.size(), 0.0); fit.Resid = 1.0;
        return fit;
    }
    std::vector<double> A(P * P, 0.0), Y(P, 0.0), b(P);
    for (size_t n = Begin; n < End; ++n) {
        for (size_t f = 0; f < Freqs.size(); ++f) {
            double w = 2 * kRPi * Freqs[f] / Fs * (double)n;
            b[2 * f] = std::cos(w); b[2 * f + 1] = std::sin(w);
        }
        b[P - 1] = 1.0;
        double y = X[n * Chs + Ch];
        for (size_t i = 0; i < P; ++i) { Y[i] += b[i] * y; for (size_t j = 0; j < P; ++j) A[i * P + j] += b[i] * b[j]; }
    }
    for (size_t i = 0; i < P; ++i) {   /* Gauss-Jordan with partial pivoting */
        size_t piv = i;
        for (size_t r = i + 1; r < P; ++r) if (std::fabs(A[r * P + i]) > std::fabs(A[piv * P + i])) piv = r;
        for (size_t k = 0; k < P; ++k) std::swap(A[i * P + k], A[piv * P + k]);
        std::swap(Y[i], Y[piv]);
        for (size_t r = 0; r < P; ++r) {
            if (r == i) continue;
            double m = A[r * P + i] / A[i * P + i];
            for (size_t k = i; k < P; ++k) A[r * P + k] -= m * A[i * P + k];
            Y[r] -= m * Y[i];
        }
    }
    std::vector<double> c(P);
    for (size_t i = 0; i < P; ++i) c[i] = Y[i] / A[i * P + i];
    fit.Resid = 0;
    for (size_t f = 0; f < Freqs.size(); ++f) { fit.Amp.push_back(std::hypot(c[2 * f], c[2 * f + 1])); fit.Phase.push_back(std::atan2(-c[2 * f + 1], c[2 * f])); }
    for (size_t n = Begin; n < End; ++n) {
        double model = c[P - 1];
        for (size_t f = 0; f < Freqs.size(); ++f) {
            double w = 2 * kRPi * Freqs[f] / Fs * (double)n;
            model += c[2 * f] * std::cos(w) + c[2 * f + 1] * std::sin(w);
        }
        double e = X[n * Chs + Ch] - model;
        fit.Resid += e * e;
    }
    fit.Resid /= (double)(End - Begin);
    return fit;
}

/* ---- the tables ----------------------------------------------------------------------- */

/* the prototype at F = Fin * L, newest-first tap order, from the shipped integers (N = CenterTwice + 1) */
static std::vector<double> RPrototype(const PN_RS_TABLE *T)
{
    std::vector<double> h((size_t)T->CenterTwice + 1, 0.0);
    for (size_t n = 0; n < h.size(); ++n) h[n] = T->Coef[(n % T->L) * T->Taps + (T->Taps - 1 - n / T->L)] / kRQ28;
    return h;
}

/* a 2:1 FIR's taps from its table (stored from the centre) */
static std::vector<double> RDec2Taps(const PN_DEC2_TABLE *D)
{
    std::vector<double> h(D->Taps, 0.0);
    h[D->Center] = D->Coef[0] / kRQ28;
    for (ULONG k = 1; k <= D->Center; ++k) h[D->Center - k] = h[D->Center + k] = D->Coef[k] / kRQ28;
    return h;
}

/* |H(f)| relative to Gain in dB; f in cycles per sample (rotation recurrence) */
static double RGainDb(const std::vector<double> &H, double F, double Gain)
{
    std::complex<double> z = std::polar(1.0, -2 * kRPi * F), w = 1.0, acc = 0.0;
    for (double v : H) { acc += v * w; w *= z; }
    return 20.0 * std::log10(PN_MAX(std::abs(acc) / Gain, 1e-300));
}

/* |H| / Gain in dB at k / P cycles per sample, k = 0 .. P / 2: a radix-2 FFT of the zero-padded
 * taps (P a power of 2), a dense grid over the whole band */
static std::vector<double> RSpectrumDb(const std::vector<double> &H, size_t P, double Gain)
{
    std::vector<std::complex<double>> a(P, 0.0);
    for (size_t i = 0; i < H.size() && i < P; ++i) a[i] = H[i];
    for (size_t i = 1, j = 0; i < P; ++i) {   /* bit-reversed order */
        size_t bit = P >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= P; len <<= 1) {
        std::complex<double> wl = std::polar(1.0, -2 * kRPi / (double)len);
        std::vector<std::complex<double>> tw(len / 2);
        tw[0] = 1.0;
        for (size_t k = 1; k < len / 2; ++k) tw[k] = (k & 63) ? tw[k - 1] * wl : std::polar(1.0, -2 * kRPi * (double)k / (double)len);
        for (size_t i = 0; i < P; i += len)
            for (size_t k = 0; k < len / 2; ++k) {
                std::complex<double> u = a[i + k], v = a[i + k + len / 2] * tw[k];
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
    std::vector<double> db(P / 2 + 1);
    for (size_t k = 0; k <= P / 2; ++k) db[k] = 20.0 * std::log10(PN_MAX(std::abs(a[k]) / Gain, 1e-300));
    return db;
}

static double RBesselI0(double x)
{
    double s = 1.0, t = 1.0;
    for (int k = 1; k < 500; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; if (t < 1e-17 * s) break; }
    return s;
}

/* Kaiser-windowed sinc of the generator (build/gen-resampler-coeffs.py): N taps, cutoff Fc cycles per
 * sample, design attenuation Atten dB (123; the cascades' short polyphase stage 1: 150) */
static std::vector<double> RKaiserSinc(size_t N, double Fc, double Gain, double Atten = 123.0)
{
    const double beta = 0.1102 * (Atten - 8.7), i0b = RBesselI0(beta);
    const size_t c = (N - 1) / 2;
    std::vector<double> h(N);
    for (size_t n = 0; n < N; ++n) {
        double x = (double)n - (double)c, r = x / (double)c;
        double s = x == 0 ? 2 * Fc : std::sin(2 * kRPi * Fc * x) / (kRPi * x);
        h[n] = Gain * s * RBesselI0(beta * std::sqrt(PN_MAX(0.0, 1 - r * r))) / i0b;
    }
    return h;
}

/* Passband ripple 0..20 kHz and the worst stopband level from Stop Hz to Rate / 2 of taps H at
 * Rate: the FFT grid (P points) plus the exact 20 kHz and Stop edges */
struct RBand { double Ripple, Hi, Lo, Stop, StopAt; };
static RBand RMeasure(const std::vector<double> &H, double Rate, double Gain, double Stop, size_t P)
{
    std::vector<double> db = RSpectrumDb(H, P, Gain);
    const double df = Rate / (double)P;
    RBand r = { 0, -1e9, 1e9, RGainDb(H, Stop / Rate, Gain), Stop };
    for (size_t k = 0; (double)k * df <= 20000.0; ++k) { r.Hi = PN_MAX(r.Hi, db[k]); r.Lo = PN_MIN(r.Lo, db[k]); }
    double edge = RGainDb(H, 20000.0 / Rate, Gain);
    r.Hi = PN_MAX(r.Hi, edge); r.Lo = PN_MIN(r.Lo, edge);
    r.Ripple = r.Hi - r.Lo;
    for (size_t k = (size_t)std::ceil(Stop / df); k <= P / 2; ++k) if (db[k] > r.Stop) { r.Stop = db[k]; r.StopAt = (double)k * df; }
    return r;
}

/* Integrity, regeneration from the design (build/gen-resampler-coeffs.py) and the response of the
 * quantised tables over the whole band. The alias-free band plan: ripple <= 0.01 dB to 20 kHz and
 * attenuation >= 110 dB (target 120 dB) from the lower of the input and output Nyquist frequencies;
 * limits here 0.0001 dB and 120 dB. */
static void TestRateConverterTables()
{
    /* the polyphase stages: input rate, stopband edge (a conversion on its own: the lower Nyquist
     * frequency, 22.05 kHz; a cascade's stage 1: 0.75 of its output rate, what folds onto the final
     * band there), prototype rate, design attenuation (stage 1 of a cascade: 150 dB, its stopband
     * holds the passband's images; asserted here at 140 dB) */
    struct RTable { const PN_RS_TABLE *T; double Fin, Stop, F, Atten, Floor; };
    const RTable tables[] = { { &g_PnRs44to48, 44100.0, 22050.0, 7056000.0, 123.0, 120.0 }, { &g_PnRs48to44, 48000.0, 22050.0, 7056000.0, 123.0, 120.0 },
                              { &g_PnRs88to96, 88200.0, 72000.0, 14112000.0, 150.0, 140.0 }, { &g_PnRs96to88, 96000.0, 66150.0, 14112000.0, 150.0, 140.0 } };
    for (const RTable &rt : tables) {
        const PN_RS_TABLE *t = rt.T;
        const double Fin = rt.Fin, F = Fin * t->L, stop = rt.Stop;
        /* the length rule: K L taps, less one when even (odd, linear phase) */
        const ULONG n = t->L * t->Taps - ((t->L * t->Taps) % 2 == 0 ? 1 : 0);
        CHECK(t->Taps <= PN_RS_MAX_TAPS && t->CenterTwice == n - 1 && F == rt.F);
        /* every phase sums to exactly 2^28 and stays within the overflow bound */
        BOOLEAN sums = TRUE, bound = TRUE;
        double maxAbs = 0;
        for (ULONG p = 0; p < t->L; ++p) {
            LONGLONG s = 0, a = 0;
            for (ULONG j = 0; j < t->Taps; ++j) { s += t->Coef[p * t->Taps + j]; a += std::abs((LONGLONG)t->Coef[p * t->Taps + j]); }
            sums = sums && s == (1ll << 28);
            bound = bound && a <= (7ll << 27);   /* 3.5 * 2^28 */
            maxAbs = PN_MAX(maxAbs, a / kRQ28);
        }
        CHECK(sums && bound);
        /* linear phase: the prototype mirrors about its centre within the residual */
        std::vector<double> h = RPrototype(t);
        double asym = 0;
        for (size_t i = 0; i < h.size(); ++i) asym = PN_MAX(asym, std::fabs(h[i] - h[h.size() - 1 - i]) * kRQ28);
        CHECK(asym <= t->Taps / 2 + 1);
        /* regeneration: Kaiser-windowed sinc, passband 20 kHz, cutoff at the transition band's centre,
         * the Kaiser length estimate, each phase scaled to 2^28 */
        const size_t N = h.size();
        const double df = (stop - 20000.0) / F;
        CHECK(t->Taps == (ULONG)std::ceil(((rt.Atten - 7.95) / (14.36 * df) + 1.0) / t->L));
        std::vector<double> ideal = RKaiserSinc(N, (20000.0 + stop) / 2 / F, t->L, rt.Atten);
        BOOLEAN regen = TRUE;
        for (ULONG p = 0; p < t->L; ++p) {
            double sum = 0;
            for (ULONG k = 0; k < t->Taps; ++k) if (p + (size_t)t->L * k < N) sum += ideal[p + (size_t)t->L * k];
            ULONG off = 0;
            for (ULONG k = 0; k < t->Taps; ++k) {
                double want = p + (size_t)t->L * k < N ? ideal[p + (size_t)t->L * k] / sum * kRQ28 : 0.0;
                double d = std::fabs(t->Coef[p * t->Taps + (t->Taps - 1 - k)] - want);
                if (d > 1.5) { off++; regen = regen && d <= t->Taps / 2 + 1; }
            }
            regen = regen && off <= 1;
        }
        CHECK(regen);
        /* response of the integers over the whole band: 2^20 points (6.7 / 13.5 Hz apart) */
        RBand r = RMeasure(h, F, t->L, stop, (size_t)1 << 20);
        printf("rate converter %u/%u from %.1f kHz: %u taps per phase (N %zu), passband ripple %.6f dB to 20 kHz, stopband %.1f dB from "
               "%.2f kHz (worst at %.0f Hz), max phase sum |h| %.3f\n", t->L, t->M, Fin / 1000.0, t->Taps, N, r.Ripple, -r.Stop, stop / 1000.0,
               r.StopAt, maxAbs);
        CHECK(r.Ripple <= 0.0001 && std::fabs(r.Hi) <= 0.0001 && std::fabs(r.Lo) <= 0.0001);
        CHECK(r.Stop <= -rt.Floor);
    }
    /* the 2:1 FIRs (the 2:1 pairs, and stage 2 of the cascades): DC exact, bound, symmetric by
     * layout, regeneration, ripple and stopband from fs_out / 2 (24 kHz from 96 kHz, 22.05 kHz from
     * 88.2 kHz) */
    for (const PN_DEC2_TABLE *d : { &g_PnDec96, &g_PnDec88 }) {
        const double Fin = d == &g_PnDec96 ? 96000.0 : 88200.0, stopHz = Fin / 4;
        LONGLONG sum = d->Coef[0], abs = std::abs((LONGLONG)d->Coef[0]);
        for (ULONG k = 1; k <= d->Center; ++k) { sum += 2ll * d->Coef[k]; abs += 2ll * std::abs((LONGLONG)d->Coef[k]); }
        CHECK(sum == (1ll << 28) && abs <= (7ll << 27) && d->Taps == 2 * d->Center + 1 && d->Taps <= PN_DEC2_MAX_TAPS);
        const double df = (stopHz - 20000.0) / Fin;
        ULONG want = (ULONG)std::ceil((123.0 - 7.95) / (14.36 * df) + 1.0);
        CHECK(d->Taps == (want % 2 ? want : want + 1));
        std::vector<double> ideal = RKaiserSinc(d->Taps, (20000.0 + stopHz) / 2 / Fin, 1.0);
        double total = 0;
        for (double v : ideal) total += v;
        ULONG off = 0;
        BOOLEAN regen = TRUE;
        for (ULONG k = 0; k <= d->Center; ++k) {
            double dv = std::fabs(d->Coef[k] - ideal[d->Center + k] / total * kRQ28);
            if (dv > 1.5) { off++; regen = regen && k == 0 && dv <= d->Center + 1; }   /* the residual sits on the centre */
        }
        CHECK(regen && off <= 1);
        RBand r = RMeasure(RDec2Taps(d), Fin, 1.0, stopHz, (size_t)1 << 16);
        printf("rate converter 2:1 from %.1f kHz: %u taps, passband ripple %.6f dB to 20 kHz, stopband %.1f dB from %.2f kHz (worst at %.0f Hz), "
               "sum |h| %.3f\n", Fin / 1000.0, d->Taps, r.Ripple, -r.Stop, stopHz / 1000.0, r.StopAt, abs / kRQ28);
        CHECK(r.Ripple <= 0.0001 && std::fabs(r.Hi) <= 0.0001 && std::fabs(r.Lo) <= 0.0001);
        CHECK(r.Stop <= -120.0);
    }
}

/* ---- the plan and the framing ------------------------------------------------------------- */

static void TestRateConverterPlan()
{
    for (const RPair &p : kRPairs) {
        PN_PCM_RATE_PLAN plan;
        CHECK(PnPcmRatePlan(p.Engine, p.Codec, &plan) && plan.EngineRate == p.Engine && plan.CodecRate == p.Codec &&
              plan.Decimate == p.Decimate && plan.Rs == p.Rs && plan.DelayEngineFrames == p.Delay);
    }
    for (ULONG r : { 8000u, 16000u, 32000u, 44100u, 48000u, 88200u, 96000u, 176400u, 192000u }) {
        PN_PCM_RATE_PLAN plan;
        CHECK(PnPcmRatePlan(r, r, &plan) && plan.Decimate == 1 && !plan.Rs && plan.DelayEngineFrames == 0);
    }
    /* the policy's PnRateConvertible (audio_policy.h) is exactly the set the converter plans */
    {
        static const ULONG grid[] = { 8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000, 64000, 88200, 96000, 176400, 192000 };
        BOOLEAN same = TRUE;
        ULONG bridged = 0;
        for (ULONG e : grid) for (ULONG c : grid) {
            PN_PCM_RATE_PLAN pl;
            BOOLEAN planned = PnPcmRatePlan(e, c, &pl);
            same = same && (PnRateConvertible(e, c, PN_RATECONV_ANY) != 0) == (planned != 0);
            if (planned && e != c) bridged++;
        }
        CHECK(same && bridged == 6);
    }
    const ULONG rejected[][2] = { { 32000, 48000 }, { 44100, 96000 }, { 192000, 48000 }, { 48000, 88200 }, { 192000, 96000 }, { 32000, 16000 },
                                  { 48000, 32000 }, { 0, 48000 }, { 48000, 0 } };
    for (const auto &r : rejected) {
        PN_PCM_RATE_PLAN plan;
        plan.DelayEngineFrames = 7;
        CHECK(!PnPcmRatePlan(r[0], r[1], &plan) && !plan.Rs && plan.DelayEngineFrames == 0 && plan.EngineRate == 0);
    }
}

/* Exact framing: PnPcmInputFramesFor(K) frames with room for K make exactly K frames and leave
 * the 2:1 stage between pairs; the frames stay within PnPcmMaxInputFrames; the totals keep the
 * rate ratio; and the framed stream equals the same input converted in one call. */
static void TestRateConverterFraming()
{
    for (const RPair &p : kRPairs) {
        RConv framed(p.Engine, p.Codec);
        RRand rnd(0xF4A3u + p.Engine + p.Codec);
        std::vector<LONG> in, all;
        std::vector<LONG> out(2048);
        ULONGLONG engine = 0, codec = 0;
        BOOLEAN exact = TRUE, bounded = TRUE, paired = TRUE, ratio = TRUE;
        for (int it = 0; it < 10000; ++it) {
            ULONG k = 1 + rnd.Next() % (it % 10 == 0 ? 2048u : 64u);
            ULONG need = PnPcmInputFramesFor(&framed.St, k);
            bounded = bounded && need <= PnPcmMaxInputFrames(&framed.Plan, k);
            in.resize(need);
            for (LONG &v : in) v = (LONG)(rnd.Next() & 0xFFFFFF00u) / 4;   /* -12 dBFS noise, 24-bit */
            all.insert(all.end(), in.begin(), in.end());
            ULONG bytes = PnConvertPcmEx(&framed.St, (const UCHAR *)in.data(), need * 4, 1, 32, 24, FALSE, (UCHAR *)out.data(), k * 4,
                                         PNA2DP_FMT_S32, 1, 32768);
            exact = exact && bytes == k * 4;
            paired = paired && (p.Decimate != 2 || framed.St.DecPhase == 0);
            engine += need; codec += k;
            double drift = (double)engine - (double)codec * p.Engine / p.Codec;
            ratio = ratio && std::fabs(drift) <= (p.Rs ? p.Rs->Taps : 2) + 2;
        }
        CHECK(exact && bounded && paired && ratio);
        /* the tight bound is reached: no wasted buffer space */
        RConv fresh(p.Engine, p.Codec);
        CHECK(PnPcmInputFramesFor(&fresh.St, 1) >= 1 && PnPcmMaxInputFrames(&fresh.Plan, 1) <= (p.Rs ? 2u : 1u) * p.Decimate);
        /* the same input in one call: identical output (no block-boundary effect) */
        RConv once(p.Engine, p.Codec);
        std::vector<UCHAR> ref = once.Run((const UCHAR *)all.data(), (ULONG)all.size(), 1, 32, 24, FALSE, PNA2DP_FMT_S32, 1);
        RConv again(p.Engine, p.Codec);
        RRand r2(0xF4A3u + p.Engine + p.Codec);
        std::vector<UCHAR> cat;
        size_t at = 0;
        for (int it = 0; it < 10000; ++it) {
            ULONG k = 1 + r2.Next() % (it % 10 == 0 ? 2048u : 64u);
            ULONG need = PnPcmInputFramesFor(&again.St, k);
            for (ULONG i = 0; i < need; ++i) r2.Next();
            std::vector<UCHAR> o(k * 4);
            ULONG bytes = PnConvertPcmEx(&again.St, (const UCHAR *)&all[at], need * 4, 1, 32, 24, FALSE, o.data(), k * 4, PNA2DP_FMT_S32, 1, 32768);
            cat.insert(cat.end(), o.begin(), o.begin() + bytes);
            at += need;
        }
        CHECK(cat.size() <= ref.size() && memcmp(cat.data(), ref.data(), cat.size()) == 0);
        /* an output buffer smaller than what the input completes consumes exactly the input of the
         * frames written; the rest of the stream then continues seamlessly */
        RConv capped(p.Engine, p.Codec);
        ULONG first = 1000;
        std::vector<UCHAR> o(first * 4 * 8);
        ULONG wantIn = PnPcmInputFramesFor(&capped.St, 100);
        ULONG bytes = PnConvertPcmEx(&capped.St, (const UCHAR *)all.data(), first * 4, 1, 32, 24, FALSE, o.data(), 100 * 4, PNA2DP_FMT_S32, 1, 32768);
        CHECK(bytes == 100 * 4 && wantIn < first && memcmp(o.data(), ref.data(), bytes) == 0);
        std::vector<UCHAR> rest = capped.Run((const UCHAR *)&all[wantIn], 20000, 1, 32, 24, FALSE, PNA2DP_FMT_S32, 1);
        CHECK(rest.size() >= 4000 && memcmp(rest.data(), ref.data() + bytes, rest.size()) == 0);
    }
}

/* ---- exactness against a model of the definition ------------------------------------------ */

/* The converter as a double-precision model of its own definition: polyphase output n over inputs
 * q - Taps + 1 .. q, q = floor(n M / L), phase n M mod L (limited to +-4.0 before a 2:1 stage);
 * 2:1 output m over its inputs 2m + 2 - Taps .. 2m + 1. Mono, full scale 1.0. */
static std::vector<double> RModel(const RPair &P, const std::vector<double> &X, size_t Outputs)
{
    std::vector<double> s = X;
    if (P.Rs) {
        const size_t mids = P.Decimate == 2 ? 2 * Outputs + 2 : Outputs;
        s.assign(mids, 0.0);
        for (size_t n = 0; n < mids; ++n) {
            ULONGLONG q = (ULONGLONG)n * P.Rs->M / P.Rs->L, ph = (ULONGLONG)n * P.Rs->M % P.Rs->L;
            double acc = 0;
            for (ULONG j = 0; j < P.Rs->Taps; ++j) {
                long long i = (long long)q - (long long)P.Rs->Taps + 1 + j;
                acc += P.Rs->Coef[ph * P.Rs->Taps + j] / kRQ28 * (i < 0 || (size_t)i >= X.size() ? 0.0 : X[(size_t)i]);
            }
            s[n] = P.Decimate == 2 ? PN_MAX(-4.0, PN_MIN(4.0, acc)) : acc;
        }
    }
    if (P.Decimate != 2) { s.resize(PN_MIN(s.size(), Outputs)); return s; }
    auto x = [&](long long i) { return i < 0 || (size_t)i >= s.size() ? 0.0 : s[(size_t)i]; };
    std::vector<double> y(PN_MIN(s.size() / 2, Outputs));
    for (size_t m = 0; m < y.size(); ++m) {
        long long c = 2 * (long long)m + 1 - (long long)P.Dec->Center;   /* the window's centre */
        double acc = P.Dec->Coef[0] / kRQ28 * x(c);
        for (ULONG k = 1; k <= P.Dec->Center; ++k) acc += P.Dec->Coef[k] / kRQ28 * (x(c - k) + x(c + k));
        y[m] = acc;
    }
    return y;
}

/* Every pair against the model: -6 dBFS noise in random blocks (within 3 LSB of S32), and a
 * +-4.0 float worst case (over the +12 dBFS limit too), where every output must saturate with the
 * model's sign: the 64-bit accumulators never wrap. */
static void TestRateConverterModel()
{
    for (const RPair &p : kRPairs) {
        RRand rnd(0xA11CEu + p.Engine * 3 + p.Codec);
        const ULONG frames = 24000;
        std::vector<LONG> in(frames);
        std::vector<double> x(frames);
        for (ULONG i = 0; i < frames; ++i) { in[i] = (LONG)rnd.Next() / 2; x[i] = in[i] / 2147483648.0; }
        RConv cv(p.Engine, p.Codec);
        std::vector<double> y = RDecode(cv.Run((const UCHAR *)in.data(), frames, 1, 32, 32, FALSE, PNA2DP_FMT_S32, 1, 32768, 997, &rnd), PNA2DP_FMT_S32);
        std::vector<double> m = RModel(p, x, y.size());
        double worst = 0;
        for (size_t n = 0; n < y.size(); ++n) if (std::fabs(m[n]) < 0.99) worst = PN_MAX(worst, std::fabs(y[n] - m[n]) * 2147483648.0);
        CHECK(y.size() + 3 >= (size_t)((double)frames * p.Codec / p.Engine) && worst <= 3.0);
        /* the worst case: random +-4.0 (and +-5.0, held to 4.0) float samples */
        std::vector<float> f(frames);
        std::vector<double> xf(frames);
        for (ULONG i = 0; i < frames; ++i) {
            ULONG r = rnd.Next();
            f[i] = (r & 1) ? ((r & 2) ? 4.0f : 5.0f) : ((r & 2) ? -4.0f : -5.0f);
            xf[i] = f[i] > 4.0f ? 4.0 : f[i] < -4.0f ? -4.0 : f[i];
        }
        RConv over(p.Engine, p.Codec);
        std::vector<UCHAR> ob = over.Run((const UCHAR *)f.data(), frames, 1, 32, 32, TRUE, PNA2DP_FMT_S32, 1, 32768, 512, &rnd);
        std::vector<double> mf = RModel(p, xf, ob.size() / 4);
        BOOLEAN saturates = TRUE;
        ULONG clipped = 0;
        for (size_t n = 0; n < mf.size(); ++n) {
            LONG v;
            memcpy(&v, &ob[n * 4], 4);
            if (mf[n] >= 1.0 + 1e-6) { saturates = saturates && v == 2147483647L; clipped++; }
            else if (mf[n] <= -1.0 - 1e-6) { saturates = saturates && v == (-2147483647L - 1); clipped++; }
            else if (std::fabs(mf[n]) < 1.0 - 1e-6) saturates = saturates && std::fabs(v / 2147483648.0 - mf[n]) * 2147483648.0 <= 8.0;
        }
        CHECK(saturates && clipped > mf.size() / 2);
        printf("rate converter %u -> %u Hz against its model: %zu outputs within %.2f LSB of S32; +-4.0 worst case: %u of %zu outputs saturate, %s\n",
               p.Engine, p.Codec, y.size(), worst, clipped, mf.size(), saturates ? "every one with the model's sign" : "MISMATCH");
    }
}

/* ---- audio measurements ---------------------------------------------------------------------- */

/* A tone at the engine rate, quantised to 24 bits with TPDF dither (S24-in-32), or as float. */
static std::vector<UCHAR> RTone(double Fs, double Freq, double Amp, ULONG Frames, ULONG Ch, BOOLEAN AsFloat, ULONG Seed)
{
    std::vector<UCHAR> b((size_t)Frames * Ch * 4);
    RRand r(Seed);
    for (ULONG n = 0; n < Frames; ++n) {
        double v = Amp * std::sin(2 * kRPi * Freq * n / Fs);
        for (ULONG c = 0; c < Ch; ++c) {
            if (AsFloat) { float fv = (float)v; memcpy(&b[((size_t)n * Ch + c) * 4], &fv, 4); continue; }
            double q = std::floor(v * 8388608.0 + r.Tpdf() + 0.5);
            LONG s = (LONG)PN_MAX(-8388608.0, PN_MIN(8388607.0, q)) * 256;
            memcpy(&b[((size_t)n * Ch + c) * 4], &s, 4);
        }
    }
    return b;
}

/* THD+N of a 997 Hz -1 dBFS tone (24-bit TPDF to S32 and to rounded S24, float to S32), its level,
 * tones across the passband, alias and image rejection, the group delay and the latency the plan
 * reports. Specification: THD+N <= -110 dB, ripple <= 0.01 dB, rejection >= 110 dB. */
static void TestRateConverterAudio()
{
    const double amp = std::pow(10.0, -1.0 / 20.0);
    double worstThd = -1e9, worstThd24 = -1e9, worstThdF = -1e9, worstLevel = 0, worstTone = 0, worstReject = -1e9;
    for (const RPair &p : kRPairs) {
        const double fin = p.Engine, fout = p.Codec;
        const ULONG frames = (ULONG)(1.2 * fin);
        size_t b = (size_t)(0.1 * fout), e = (size_t)(1.1 * fout);
        /* THD+N: 24-bit TPDF input; the dither alone is -140.5 dB re the tone */
        for (ULONG target : { (ULONG)PNA2DP_FMT_S32, (ULONG)PNA2DP_FMT_S24 }) for (int floatIn = 0; floatIn < 2; ++floatIn) {
            const BOOLEAN asFloat = floatIn != 0;
            if (asFloat && target != PNA2DP_FMT_S32) continue;
            std::vector<UCHAR> in = RTone(fin, 997.0, amp, frames, 2, asFloat, 0x7E57u + p.Engine);
            RConv cv(p.Engine, p.Codec);
            std::vector<double> y = RDecode(cv.Run(in.data(), frames, 2, 32, asFloat ? 32 : 24, asFloat, target, 2, 32768, 1500, NULL), target);
            for (ULONG c = 0; c < 2; ++c) {
                RFit f = RFitTones(y, 2, c, b, e, fout, { 997.0 });
                double thd = 10 * std::log10(f.Resid / (f.Amp[0] * f.Amp[0] / 2));
                if (asFloat) worstThdF = PN_MAX(worstThdF, thd);
                else if (target == PNA2DP_FMT_S24) worstThd24 = PN_MAX(worstThd24, thd);
                else worstThd = PN_MAX(worstThd, thd);
                worstLevel = PN_MAX(worstLevel, std::fabs(20 * std::log10(f.Amp[0] / amp)));
            }
        }
        /* tones across the passband (S32 input, 0.5 FS): level within 0.005 dB */
        for (double tone : { 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 19000.0, 20000.0 }) {
            std::vector<LONG> in(frames);
            for (ULONG n = 0; n < frames; ++n) in[n] = (LONG)std::lround(0.5 * 2147483648.0 * std::sin(2 * kRPi * tone * n / fin));
            RConv cv(p.Engine, p.Codec);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)in.data(), frames, 1, 32, 32, FALSE, PNA2DP_FMT_S32, 1), PNA2DP_FMT_S32);
            RFit f = RFitTones(y, 1, 0, b, e, fout, { tone });
            worstTone = PN_MAX(worstTone, std::fabs(20 * std::log10(f.Amp[0] / 0.5)));
        }
        /* alias (a tone above the output band) and image (upsampling) rejection: the component at
         * the frequency it folds to, fitted jointly with the tone itself */
        struct Probe { double Tone, At; };
        std::vector<Probe> probes;
        if (p.Engine == 96000 && p.Codec == 48000) probes.push_back({ 30000.0, 18000.0 });
        if (p.Engine == 88200 && p.Codec == 44100) probes.push_back({ 27500.0, 16600.0 });
        /* the cascades: the polyphase stage passes 30 kHz into the 2:1 stage, which folds it about the
         * final Nyquist frequency; 19 kHz's first image (Engine - 19 kHz) folds at Mid, then again */
        if (p.Engine == 96000 && p.Codec == 44100) { probes.push_back({ 30000.0, 44100.0 - 30000.0 }); probes.push_back({ 19000.0, 88200.0 - (96000.0 - 19000.0) }); }
        if (p.Engine == 44100 && p.Codec == 48000) probes.push_back({ 19000.0, 48000.0 - (44100.0 - 19000.0) });
        if (p.Engine == 88200 && p.Codec == 48000) { probes.push_back({ 30000.0, 48000.0 - 30000.0 }); probes.push_back({ 19000.0, 48000.0 - (96000.0 - (88200.0 - 19000.0)) }); }
        for (const Probe &pr : probes) {
            std::vector<LONG> in(frames);
            for (ULONG n = 0; n < frames; ++n) in[n] = (LONG)std::lround(0.5 * 2147483648.0 * std::sin(2 * kRPi * pr.Tone * n / fin));
            RConv cv(p.Engine, p.Codec);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)in.data(), frames, 1, 32, 32, FALSE, PNA2DP_FMT_S32, 1), PNA2DP_FMT_S32);
            std::vector<double> fr = { pr.At };
            if (pr.Tone < fout / 2) fr.push_back(pr.Tone);
            RFit f = RFitTones(y, 1, 0, b, e, fout, fr);
            double rej = 20 * std::log10(PN_MAX(f.Amp[0], 1e-300) / 0.5);
            worstReject = PN_MAX(worstReject, rej);
            CHECK(rej <= -110.0);
        }
        /* group delay from the phase of a 100 Hz tone (cosine phase at output 0, nominal time
         * n / Fout): (polyphase ? CenterTwice / (2 L) : 0) engine frames + (2:1 stage ? Center - 1 : 0)
         * of its input frames (Engine / Mid engine frames each in a cascade); the plan's latency
         * (which adds the 2:1 stage's wait for its second frame) within 1.5 frames of it */
        {
            std::vector<LONG> in(frames);
            for (ULONG n = 0; n < frames; ++n) in[n] = (LONG)std::lround(0.5 * 2147483648.0 * std::cos(2 * kRPi * 100.0 * n / fin));
            RConv cv(p.Engine, p.Codec);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)in.data(), frames, 1, 32, 32, FALSE, PNA2DP_FMT_S32, 1), PNA2DP_FMT_S32);
            RFit f = RFitTones(y, 1, 0, b, e, fout, { 100.0 });
            double tauFrames = -f.Phase[0] / (2 * kRPi * 100.0) * fin;
            while (tauFrames < 0) tauFrames += fin / 100.0;
            double design = (p.Rs ? (double)p.Rs->CenterTwice / (2.0 * p.Rs->L) : 0.0) +
                            (p.Dec ? (p.Dec->Center - 1.0) * (p.Rs ? fin / RMid(p) : 1.0) : 0.0);
            CHECK(std::fabs(tauFrames - design) <= 0.01);
            CHECK(std::fabs((double)p.Delay - tauFrames) <= 1.5);
            printf("rate converter %u -> %u Hz: group delay %.4f engine frames (%.3f ms), plan latency %u frames\n", p.Engine, p.Codec, tauFrames,
                   tauFrames * 1000.0 / fin, p.Delay);
        }
    }
    printf("rate converter: 997 Hz -1 dBFS THD+N %.1f dB (24-bit TPDF in, S32 out), %.1f dB (S24 out, rounded), %.1f dB (float in), "
           "level error %.5f dB; tones 100 Hz..20 kHz within %.5f dB; alias / image rejection %.1f dB\n",
           worstThd, worstThd24, worstThdF, worstLevel, worstTone, -worstReject);
    CHECK(worstThd <= -125.0);
    CHECK(worstThd24 <= -120.0);
    CHECK(worstThdF <= -130.0);
    CHECK(worstLevel <= 0.001);
    CHECK(worstTone <= 0.005);
}

/* Where a tone of Hz at Rate lands after sampling at Rate: folded into 0 .. Rate / 2. */
static double RFold(double Hz, double Rate)
{
    double f = std::fmod(Hz, Rate);
    if (f < 0) f += Rate;
    return f > Rate / 2 ? Rate - f : f;
}

/* Alias-free band plan (owner decision, 14 October 2026; review #2 had found the 24.1 kHz stopband
 * edge): no alias or image of any input frequency may come out above -110 dB re the tone, through
 * every converted pair. The stopband starts at the lower of the input and output Nyquist frequencies
 * (22.05 kHz whenever 44.1 kHz is involved, 24 kHz for 96 / 88.2 -> 48 kHz), so the transition band, where
 * 22.5 kHz from a 48 kHz source used to come out at 21.6 kHz only 13 dB down, no longer leaks.
 * Probes: the tones whose alias / image lands at 22.5, 23, 23.5 and 23.9 kHz where the output has
 * those frequencies (a 48 kHz output: the tones themselves pass, 25.5 .. 24.1 kHz alias onto them,
 * 21.6 .. 20.2 kHz of a 44.1 kHz source image onto them), or the tones 22.5 .. 23.9 kHz themselves
 * (a 44.1 kHz output: they alias to 21.6 .. 20.2 kHz); and a sweep of tones every 250 Hz from 20 kHz
 * to 24 kHz (26 kHz for a 48 kHz output from 88.2 / 96 kHz), below the input's Nyquist frequency.
 * Each: the main alias / image fitted jointly with the tone (when the tone itself is in the output
 * band), and everything else the converter puts out (the fit's residual: every alias, image and
 * rounding error together), both at most -110 dB re the tone. Float input and S32 output: the
 * measurement floor is far below the requirement. */
static void TestRateConverterTransition()
{
    for (const RPair &p : kRPairs) {
        const double fin = p.Engine, fout = p.Codec, mid = RMid(p);   /* mid: the polyphase stage's output rate */
        std::vector<double> tones;
        for (double t : { 22500.0, 23000.0, 23500.0, 23900.0 }) {
            if (fout == 44100.0) tones.push_back(t);                    /* aliases to 44.1 kHz - t */
            else if (fin == 44100.0) tones.push_back(44100.0 - t);      /* images onto t */
            else { tones.push_back(t); tones.push_back(fout - t); }     /* passes / aliases onto t */
        }
        const double sweepTop = PN_MIN(fout == 48000.0 && fin > 48000.0 ? 26000.0 : 24000.0, fin / 2 - 100.0);
        for (double t = 20000.0; t <= sweepTop; t += 250.0) tones.push_back(t);
        double worstMain = -400, worstRest = -400, worstMainTone = 0, worstRestTone = 0;
        for (double tone : tones) {
            const ULONG frames = (ULONG)(1.2 * fin);
            std::vector<float> in(frames);
            for (ULONG n = 0; n < frames; ++n) in[n] = (float)(0.5 * std::sin(2 * kRPi * tone * n / fin));
            RConv cv(p.Engine, p.Codec);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)in.data(), frames, 1, 32, 32, TRUE, PNA2DP_FMT_S32, 1), PNA2DP_FMT_S32);
            /* the path the tone takes: the polyphase stage folds it about mid / 2, the 2:1 stage (or
             * the output) about fout / 2; its first image lies at fin - the tone, folded the same way */
            const double s1 = p.Rs ? RFold(tone, mid) : tone;
            const bool wanted = s1 == tone && tone < fout / 2;
            double main = RFold(s1, fout);
            if (wanted) main = p.Rs ? RFold(RFold(fin - tone, mid), fout) : -1.0;   /* the first image (none after a 2:1 stage alone) */
            const bool named = main > 0 && std::fabs(main - tone) > 50.0;
            std::vector<double> fr;
            if (named) fr.push_back(main);
            if (wanted) fr.push_back(tone);
            RFit f = RFitTones(y, 1, 0, (size_t)(0.1 * fout), (size_t)(1.1 * fout), fout, fr);
            double level = named ? 20 * std::log10(PN_MAX(f.Amp[0], 1e-300) / 0.5) : -400.0;
            double rest = 10 * std::log10(PN_MAX(f.Resid, 1e-300) / (0.5 * 0.5 / 2));
            if (level > worstMain) { worstMain = level; worstMainTone = tone; }
            if (rest > worstRest) { worstRest = rest; worstRestTone = tone; }
            if (level > -110.0 || rest > -110.0)
                printf("rate converter %u -> %u Hz: %.2f kHz tone -> %.2f kHz at %.1f dB, everything else %.1f dB (need <= -110 dB)\n",
                       p.Engine, p.Codec, tone / 1000.0, main / 1000.0, level, rest);
            CHECK(level <= -110.0);
            CHECK(rest <= -110.0);
        }
        printf("rate converter %u -> %u Hz transition band: %zu tones 20..%.1f kHz, worst alias / image %.1f dB (%.2f kHz tone), "
               "everything else at most %.1f dB (%.2f kHz tone)\n", p.Engine, p.Codec, tones.size(), sweepTop / 1000.0, worstMain,
               worstMainTone / 1000.0, worstRest, worstRestTone / 1000.0);
    }
}

/* Block-size invariance and determinism: 2 s of a 997 Hz -1 dBFS tone plus white noise, in one call
 * and in random blocks of 1..4096 frames, stereo, to S32 and to dithered S16: bit-identical. */
static void TestRateConverterBlocks()
{
    for (const RPair &p : kRPairs) {
        const ULONG frames = 2 * p.Engine;
        std::vector<LONG> in((size_t)frames * 2);
        RRand rnd(0xB10Cu + p.Engine);
        for (ULONG n = 0; n < frames; ++n) {
            in[n * 2] = (LONG)std::lround(0.891 * 2147483647.0 * std::sin(2 * kRPi * 997.0 * n / p.Engine));
            in[n * 2 + 1] = (LONG)rnd.Next() / 4;
        }
        for (ULONG target : { (ULONG)PNA2DP_FMT_S32, (ULONG)PNA2DP_FMT_S16 }) {
            RConv a(p.Engine, p.Codec), b(p.Engine, p.Codec);
            a.St.Dither = b.St.Dither = TRUE;
            std::vector<UCHAR> whole = a.Run((const UCHAR *)in.data(), frames, 2, 32, 32, FALSE, target, 2, 23198);
            std::vector<UCHAR> split = b.Run((const UCHAR *)in.data(), frames, 2, 32, 32, FALSE, target, 2, 23198, 4096, &rnd);
            CHECK(whole.size() == split.size() && whole == split);
            CHECK(whole.size() / (2 * (target == PNA2DP_FMT_S16 ? 2 : 4)) + 3 >= (size_t)frames * p.Codec / p.Engine);
        }
    }
}

/* The 2:1 decimator tests of 0.1.0.21, through every converting pair: DC exact, digital silence stays
 * exact zeros (dither on), a signal's tail decays to exact zero once the history is empty, gain
 * ramps, a reset is a fresh filter, a trim crossfade counts engine frames, NaN concealment, a
 * fade-in start, and TPDF dither keeps a 0.3 LSB DC. */
static void TestRateConverterStates()
{
    for (const RPair &p : kRPairs) {
        PN_PCM_RATE_PLAN plan;
        CHECK(PnPcmRatePlan(p.Engine, p.Codec, &plan));
        /* DC: exact once the filter has filled (every phase sums to 2^28) */
        {
            std::vector<SHORT> dc(8192 * 2, 16000);
            RConv cv(p.Engine, p.Codec);
            std::vector<UCHAR> o = cv.Run((const UCHAR *)dc.data(), 8192, 2, 16, 16, FALSE, PNA2DP_FMT_S16, 2);
            BOOLEAN exact = o.size() >= 3000 * 4;
            for (size_t i = 1000 * 2; i < o.size() / 2; ++i) { SHORT v; memcpy(&v, &o[i * 2], 2); exact = exact && v == 16000; }
            CHECK(exact);
        }
        /* digital silence is exact zeros, dither on; a signal's tail decays to exact zero once the
         * filter history is empty (the block that carries the tail is dithered, the next is silent) */
        {
            std::vector<SHORT> pcm(6144 * 2, 0);
            for (ULONG i = 0; i < 2048 * 2; ++i) pcm[i] = (SHORT)((i * 7919) % 20000 - 10000);
            RConv cv(p.Engine, p.Codec);
            cv.St.Dither = TRUE;
            std::vector<UCHAR> sig = cv.Run((const UCHAR *)pcm.data(), 2048, 2, 16, 16, FALSE, PNA2DP_FMT_S16, 2);
            std::vector<UCHAR> decay = cv.Run((const UCHAR *)(pcm.data() + 2048 * 2), 2048, 2, 16, 16, FALSE, PNA2DP_FMT_S16, 2);
            std::vector<UCHAR> tail = cv.Run((const UCHAR *)(pcm.data() + 4096 * 2), 2048, 2, 16, 16, FALSE, PNA2DP_FMT_S16, 2);
            BOOLEAN zero = !tail.empty(), carried = FALSE;
            for (UCHAR v : tail) zero = zero && v == 0;
            for (size_t i = 0; i < 64 && i < decay.size(); ++i) carried = carried || decay[i] != 0;
            CHECK(!sig.empty() && carried && zero);
            RConv quiet(p.Engine, p.Codec);
            quiet.St.Dither = TRUE;
            std::vector<UCHAR> z = quiet.Run((const UCHAR *)(pcm.data() + 2048 * 2), 2048, 2, 16, 16, FALSE, PNA2DP_FMT_S16, 2, 10362);
            zero = !z.empty();
            for (UCHAR v : z) zero = zero && v == 0;
            CHECK(zero);
        }
        /* a gain ramp runs across one call's output: DC 16000 from unity to -6 dB */
        {
            std::vector<SHORT> dc(8192, 16000);
            RConv cv(p.Engine, p.Codec);
            cv.Run((const UCHAR *)dc.data(), 4096, 1, 16, 16, FALSE, PNA2DP_FMT_S32, 1, 32768);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)dc.data(), 4096, 1, 16, 16, FALSE, PNA2DP_FMT_S32, 1, 16423), PNA2DP_FMT_S32);
            BOOLEAN ramp = y.size() > 100;
            for (size_t i = 0; i < y.size(); ++i) {
                LONG g = 32768 + (LONG)(((LONGLONG)(16423 - 32768) * (LONG)(i + 1)) / (LONG)y.size());
                ramp = ramp && (LONGLONG)std::llround(y[i] * 2147483648.0) == (16000ll << 16) * g / 32768;
            }
            CHECK(ramp);
        }
        /* a reset state is a fresh filter: the next stream does not hear the previous one */
        {
            std::vector<SHORT> noise(6000 * 2);
            RRand r(0x5EED);
            for (SHORT &v : noise) v = (SHORT)r.Next();
            RConv used(p.Engine, p.Codec), fresh(p.Engine, p.Codec);
            used.Run((const UCHAR *)noise.data(), 3000, 2, 16, 16, FALSE, PNA2DP_FMT_S32, 2);
            PnPcmStateResetRate(&used.St, &plan, FALSE);
            CHECK(used.Run((const UCHAR *)(noise.data() + 6000), 3000, 2, 16, 16, FALSE, PNA2DP_FMT_S32, 2) ==
                  fresh.Run((const UCHAR *)(noise.data() + 6000), 3000, 2, 16, 16, FALSE, PNA2DP_FMT_S32, 2));
        }
        /* a trim crossfade counts engine-rate frames */
        {
            std::vector<SHORT> fadeSrc(192 * 2, 1000), in(256 * 2, -1000);
            RConv cv(p.Engine, p.Codec);
            cv.St.FadeOut = (const UCHAR *)fadeSrc.data(); cv.St.FadeFrames = 192;
            std::vector<UCHAR> o(4096);
            PnConvertPcmEx(&cv.St, (const UCHAR *)in.data(), 128 * 4, 2, 16, 16, FALSE, o.data(), (ULONG)o.size(), PNA2DP_FMT_S16, 2, 32768);
            CHECK(cv.St.FadePos == 128 && cv.St.FadeFrames == 192);
            PnConvertPcmEx(&cv.St, (const UCHAR *)in.data(), 128 * 4, 2, 16, 16, FALSE, o.data(), (ULONG)o.size(), PNA2DP_FMT_S16, 2, 32768);
            CHECK(cv.St.FadeOut == NULL && cv.St.FadeFrames == 0);
        }
        /* NaN / Inf: the channel's last finite sample is held, exactly as if it were repeated */
        {
            const ULONG n = 4000;
            std::vector<float> a(n), b(n);
            for (ULONG i = 0; i < n; ++i) a[i] = b[i] = (float)(0.5 * std::sin(2 * kRPi * 997.0 * i / p.Engine));
            ULONG nanBits = 0x7fc00000, infBits = 0x7f800000;
            memcpy(&a[1000], &nanBits, 4); b[1000] = b[999];
            memcpy(&a[2500], &infBits, 4); b[2500] = b[2499];
            RConv ca(p.Engine, p.Codec), cb(p.Engine, p.Codec);
            CHECK(ca.Run((const UCHAR *)a.data(), n, 1, 32, 32, TRUE, PNA2DP_FMT_S32, 1) == cb.Run((const UCHAR *)b.data(), n, 1, 32, 32, TRUE, PNA2DP_FMT_S32, 1));
        }
        /* a mid-programme start fades in over the first call: on the input side, before the filters.
         * Ramping the output instead let the filter see a full-level onset in its empty history,
         * which comes out one filter delay later, when the ramp had already risen: a band-limited
         * step (aptX HD turned it into a +28 dB click at a converted stream restart, harness) */
        {
            std::vector<SHORT> dc(4096 * 2, 16000);
            RConv cv(p.Engine, p.Codec, TRUE);
            CHECK(cv.St.RampIn && !cv.St.GainValid && cv.St.Round24 && cv.St.RsPending == 1);
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)dc.data(), 2048, 2, 16, 16, FALSE, PNA2DP_FMT_S32, 2), PNA2DP_FMT_S32);
            std::vector<double> z = RDecode(cv.Run((const UCHAR *)dc.data(), 4096, 2, 16, 16, FALSE, PNA2DP_FMT_S32, 2), PNA2DP_FMT_S32);
            const double level = 16000.0 / 32768.0;
            double curve = 0;   /* the largest second difference: a step's edge, not a ramp's */
            for (size_t n = 1; n + 1 < y.size() / 2; ++n) curve = PN_MAX(curve, std::fabs(y[(n + 1) * 2] - 2 * y[n * 2] + y[(n - 1) * 2]));
            CHECK(!cv.St.RampIn && y.size() > 400 && std::fabs(y[0]) < 1e-3 * level && curve < 1e-3 * level);
            CHECK(z.size() > 400 && std::fabs(z[z.size() - 1] - level) < 1e-9);   /* then the programme at full level */
        }
        /* TPDF dither to S16: 0.3 LSB of DC survives on average, within +-1 LSB */
        {
            std::vector<SHORT> one(48000, 1);
            RConv cv(p.Engine, p.Codec);
            cv.St.Dither = TRUE;
            std::vector<double> y = RDecode(cv.Run((const UCHAR *)one.data(), 48000, 1, 16, 16, FALSE, PNA2DP_FMT_S16, 1, 9830), PNA2DP_FMT_S16);
            double mean = 0, worst = 0;
            for (size_t i = 500; i < y.size(); ++i) { mean += y[i] * 32768.0; worst = PN_MAX(worst, std::fabs(y[i] * 32768.0)); }
            mean /= (double)(y.size() - 500);
            CHECK(mean > 0.28 && mean < 0.32 && worst <= 1.0);
        }
        /* S24 output rounds on a converted stream: a DC of 0.75 LSB24 is 1 LSB, not truncated to 0 */
        {
            std::vector<LONG> dc(8192, 192);   /* 0.75 of 256 */
            RConv cv(p.Engine, p.Codec);
            std::vector<UCHAR> o = cv.Run((const UCHAR *)dc.data(), 8192, 1, 32, 32, FALSE, PNA2DP_FMT_S24, 1);
            BOOLEAN rounded = o.size() > 3000 * 3;
            for (size_t i = 1000; i < o.size() / 3; ++i) rounded = rounded && o[i * 3] == 1 && o[i * 3 + 1] == 0 && o[i * 3 + 2] == 0;
            CHECK(rounded);
        }
    }
    /* the native path keeps truncating S24 (bit-exact with 0.1.0.21) */
    {
        LONG dc[4] = { 192, 192, -192, 255 };
        UCHAR o[12];
        PN_PCM_STATE st;
        PnPcmStateResetRate(&st, NULL, FALSE);
        CHECK(!st.Round24 && !st.Rs && st.Decimate == 1);
        CHECK(PnConvertPcmEx(&st, (const UCHAR *)dc, sizeof(dc), 1, 32, 32, FALSE, o, sizeof(o), PNA2DP_FMT_S24, 1, 32768) == 12);
        CHECK(o[0] == 0 && o[3] == 0 && o[6] == 0xff && o[9] == 0);
    }
}

/* A pause or end of stream (PumpFinishStream): the rendered PCM fades, is padded (PnFinishPadBytes:
 * to whole blocks, plus PnFinishExtraBlocks blocks) and the pump takes blocks of PnPcmInputFramesFor
 * frames while the ring holds one. Required once it stops:
 *   (a) every rendered frame is out of the converter: consumed >= rendered + DelayEngineFrames;
 *   (b) the encoder has been fed its own delay past it, so the receiver plays the whole fade before
 *       the stream goes idle: consumed >= rendered + DelayEngineFrames + the codec's delay in engine
 *       frames (encode + decode, as the quality harness calibrates them: SBC 73 codec frames, 37
 *       with 4 subbands, aptX 90, FDK AAC 1600; rounded up, plus one frame for the rounded plan);
 *   (c) every codec (LDAC too): at least the native path's flush (base blocks of K codec frames)
 *       follows the converter's delay.
 * The pump takes every block whose input the ring holds, so it stops at the largest cumulative
 * input C[n] <= the padded total; C is simulated with the production framing, the state advanced
 * as PnConvertPcmEx advances it (checked against it first). Swept: every converter phase a stream
 * can reach (L start phases), every backlog of whole blocks in the 600 ms ring up to the latency
 * guard's 500 ms ceiling (the ring's 100 ms headroom above it takes the padding, or
 * PnFinishPadBytes cuts it to the room), and the residuals that bound the margin (0, 1, block - 1:
 * the padded total depends only on the backlog and on whether a residual is waiting; and 500 ms).
 * Regressions it holds: one extra block (0.1.0.21) left polyphase streams up to a block short (the
 * sound ended mid-fade, the rest came out after the resume); with the alias-free filters' longer
 * delays the first 0.1.0.22 rule (one more block for a polyphase stage only) left the last ~0.7 ms
 * of the fade in the SBC encoder with one SBC frame per packet (88.2 -> 44.1 kHz: a quarter of
 * pauses), and at 88.2 -> 48 kHz with a backlog above ~415 ms rendered audio in the converter
 * (review of 0.1.0.22, #1; the first alias-free build's 2:1-first cascades held 346 / 285 frames). */
static void TestRateConverterPauseDrain()
{
    struct DrainCodec { ULONG Id, Delay; std::vector<ULONG> K; const char *Name; };
    std::vector<ULONG> aptxK;
    for (ULONG k = 128; k <= 640; k += 32) aptxK.push_back(k);   /* 8 codewords of 4 frames per step */
    const DrainCodec codecs[] = {
        { PNA2DP_CODEC_SBC, 73, { 128, 256, 384, 512, 640, 768, 896, 1024 }, "SBC" },   /* 16 blocks x 8 subbands, n per packet */
        { PNA2DP_CODEC_SBC, 37, { 64, 128, 192, 256, 320, 384 }, "SBC 4 subbands" },
        { PNA2DP_CODEC_APTX, 90, aptxK, "aptX" },
        { PNA2DP_CODEC_LDAC, 0, { 256, 512 }, "LDAC" },                                 /* 2 frames of 128 / 256 */
        { PNA2DP_CODEC_AAC, 1600, { 1024 }, "AAC" },
    };
    /* the rule under test and the rules it replaces */
    auto ruleNow = [](ULONG Codec, const PN_PCM_RATE_PLAN &Plan, ULONG Block) { return PnFinishExtraBlocks(Codec, Block, Plan.DelayEngineFrames); };
    auto rule021 = [](ULONG Codec, const PN_PCM_RATE_PLAN &, ULONG) { return Codec == PNA2DP_CODEC_AAC ? 2u : 1u; };
    auto ruleDraft = [](ULONG Codec, const PN_PCM_RATE_PLAN &Plan, ULONG) { return (Codec == PNA2DP_CODEC_AAC ? 2u : 1u) + (Plan.Rs ? 1u : 0u); };
    ULONGLONG cases = 0, cut = 0;
    ULONG failConv = 0, failCodec = 0, failFlush = 0;          /* the rule under test: (a), (b), (c) */
    ULONG draftConv = 0, draftCodec = 0, draftSbc128 = 0, oldConv = 0;
    double worstCodecMargin = 1e9;                             /* codec frames past (b), the rule under test */
    for (const RPair &p : kRPairs) {
        PN_PCM_RATE_PLAN plan;
        CHECK(PnPcmRatePlan(p.Engine, p.Codec, &plan));
        /* the state after a block of K outputs from exactly its input, as the converter leaves it
         * (Decimate * K polyphase outputs; the 2:1 stage between pairs) */
        auto advance = [&](PN_PCM_STATE &s, ULONG K) {
            if (p.Rs) {
                ULONGLONG n = (ULONGLONG)K * p.Decimate;
                ULONG before = (ULONG)(((ULONGLONG)s.RsPhase + (n - 1) * p.Rs->M) % p.Rs->L);
                s.RsPending = (before + p.Rs->M) / p.Rs->L;
                s.RsPhase = (ULONG)(((ULONGLONG)s.RsPhase + n * p.Rs->M) % p.Rs->L);
            }
            s.DecPhase = 0;
        };
        {   /* the replica against the converter: 300 blocks of random size */
            RConv cv(p.Engine, p.Codec);
            PN_PCM_STATE sim;
            PnPcmStateResetRate(&sim, &plan, FALSE);
            std::vector<SHORT> zero(8192, 0), out(4096);
            RRand r(0xD2A1u + p.Engine);
            BOOLEAN same = TRUE;
            for (int i = 0; i < 300; ++i) {
                ULONG k = 1 + r.Next() % 700, need = PnPcmInputFramesFor(&cv.St, k);
                same = same && need == PnPcmInputFramesFor(&sim, k) &&
                       PnConvertPcmEx(&cv.St, (const UCHAR *)zero.data(), need * 2, 1, 16, 16, FALSE, (UCHAR *)out.data(), k * 2, PNA2DP_FMT_S16, 1, 32768) == k * 2;
                advance(sim, k);
                same = same && sim.RsPending == cv.St.RsPending && sim.RsPhase == cv.St.RsPhase && sim.DecPhase == cv.St.DecPhase;
            }
            CHECK(same);
        }
        const ULONG ring = (ULONG)((ULONGLONG)p.Engine * 600 / 1000);   /* PN_PUMP_RING_MS of frames */
        /* the most the ring holds at a pause: the latency guard's ceiling (config_policy.h) */
        const ULONG limit = (ULONG)((ULONGLONG)p.Engine * PN_CONFIG_MAX_LATENCY_MS / 1000);
        double pairMargin = 1e9;
        ULONG pairSbc128Draft = 0;
        for (const DrainCodec &cd : codecs) for (ULONG K : cd.K) {
            const ULONG block = PnPcmMaxInputFrames(&plan, K);
            const ULONG base = rule021(cd.Id, plan, block);
            const ULONG codecEngine = cd.Delay ? (ULONG)(((ULONGLONG)cd.Delay * p.Engine + p.Codec - 1) / p.Codec) + 1 : 0;
            const ULONG flushEngine = (ULONG)((ULONGLONG)base * K * p.Engine / p.Codec);   /* the native path's flush */
            const ULONG extraNow = ruleNow(cd.Id, plan, block), extraDraft = ruleDraft(cd.Id, plan, block);
            const ULONG starts = p.Rs ? p.Rs->L : 1;
            ULONG maxBacklogMs = 0;
            std::vector<ULONG> C;
            for (ULONG start = 0; start < starts; ++start) {
                PN_PCM_STATE s;
                PnPcmStateResetRate(&s, &plan, FALSE);
                s.RsPhase = start;
                advance(s, K);   /* RsPhase (start + K M) mod L: every phase once */
                C.assign(1, 0);
                while (C.back() <= ring) { C.push_back(C.back() + PnPcmInputFramesFor(&s, K)); advance(s, K); }
                /* the pump's stop: the largest C[n] <= Total */
                auto consumedFor = [&](ULONG Total) { return *(std::upper_bound(C.begin(), C.end(), Total) - 1); };
                for (ULONG b = 0; b * block <= limit; ++b) {
                    for (ULONG residual : std::initializer_list<ULONG>{ 0, 1, block - 1, PN_MIN(limit - b * block, block - 1) }) {
                        ULONG rendered = b * block + residual;
                        if (rendered > limit) continue;
                        maxBacklogMs = PN_MAX(maxBacklogMs, (ULONG)((ULONGLONG)rendered * 1000 / p.Engine));
                        cases++;
                        /* the padding cut to the ring's room (PnFinishPadBytes): the 100 ms headroom above the latency maximum */
                        cut += PnFinishPadBytes(rendered, block, extraNow, 0xFFFFFFFFu) > ring - rendered;
                        ULONG now = consumedFor(rendered + PnFinishPadBytes(rendered, block, extraNow, ring - rendered));
                        ULONG draft = consumedFor(rendered + PnFinishPadBytes(rendered, block, extraDraft, ring - rendered));
                        ULONG old = consumedFor(rendered + PnFinishPadBytes(rendered, block, base, ring - rendered));
                        ULONG conv = rendered + plan.DelayEngineFrames;
                        failConv += now < conv;
                        failFlush += now < conv + flushEngine;
                        draftConv += draft < conv;
                        oldConv += p.Rs && old < conv;
                        if (cd.Delay) {
                            failCodec += now < conv + codecEngine;
                            draftCodec += draft < conv + codecEngine;
                            if (cd.Id == PNA2DP_CODEC_SBC && cd.Delay == 73 && K == 128) pairSbc128Draft += draft < conv + codecEngine;
                            double m = ((double)now - conv - codecEngine) * p.Codec / p.Engine;
                            pairMargin = PN_MIN(pairMargin, m);
                        }
                    }
                }
            }
            CHECK(maxBacklogMs == PN_CONFIG_MAX_LATENCY_MS);   /* every backlog the latency maximum allows */
            if (cd.Id == PNA2DP_CODEC_SBC && cd.Delay == 73 && K == 128)
                printf("pause drain %u -> %u Hz, SBC 128 frames per block (%u engine frames): %u extra blocks (first 0.1.0.22 rule %u), "
                       "backlogs to %u ms\n", p.Engine, p.Codec, block, extraNow, extraDraft, maxBacklogMs);
        }
        draftSbc128 += pairSbc128Draft;
        worstCodecMargin = PN_MIN(worstCodecMargin, pairMargin);
        printf("rate converter %u -> %u Hz pause drain: smallest margin %.1f codec frames past the converter's %u-frame delay and the codec's own; "
               "first 0.1.0.22 rule, SBC 128 frames per block: %u cases short\n", p.Engine, p.Codec, pairMargin, plan.DelayEngineFrames, pairSbc128Draft);
    }
    printf("rate converter pause drain: %llu cases (backlogs up to %u ms, the padding cut to the ring's room in %llu); short of the "
           "converter %u, of the codec's delay %u, of the native flush %u (first 0.1.0.22 rule: %u / %u, SBC 128 frames per block %u; "
           "one extra block as in 0.1.0.21: %u of the converter); smallest margin %.1f codec frames\n", cases, PN_CONFIG_MAX_LATENCY_MS, cut,
           failConv, failCodec, failFlush, draftConv, draftCodec, draftSbc128, oldConv, worstCodecMargin);
    CHECK(cases > 100000 && failConv == 0 && failCodec == 0 && failFlush == 0);
    CHECK(draftCodec > 0 && draftSbc128 > 0 && draftConv > 0);   /* the defects the rule removes (review of 0.1.0.22, #1) */
    CHECK(oldConv > 0);                                         /* and 0.1.0.21's */
    /* the padding rule itself: whole blocks, within the room */
    CHECK(PnFinishPadBytes(1000, 384, 1, 100000) == 152 + 384 && PnFinishPadBytes(768, 384, 2, 100000) == 768);
    CHECK(PnFinishPadBytes(1000, 384, 1, 500) == 384 && PnFinishPadBytes(1000, 0, 1, 500) == 0);
    /* native: the codec's flush only (unchanged); converting: base + ceil((delay + block - 1) / block) */
    CHECK(PnFinishExtraBlocks(PNA2DP_CODEC_SBC, 384, 0) == 1 && PnFinishExtraBlocks(PNA2DP_CODEC_AAC, 1024, 0) == 2 &&
          PnFinishExtraBlocks(PNA2DP_CODEC_LDAC, 512, 0) == 1);
    CHECK(PnFinishExtraBlocks(PNA2DP_CODEC_SBC, 256, 173) == 3 && PnFinishExtraBlocks(PNA2DP_CODEC_SBC, 236, 346) == 4 &&
          PnFinishExtraBlocks(PNA2DP_CODEC_SBC, 236, 98) == 3 &&
          PnFinishExtraBlocks(PNA2DP_CODEC_AAC, 941, 86) == 4 && PnFinishExtraBlocks(PNA2DP_CODEC_SBC, 0, 86) == 1);
}

/* CPU cost per second of stereo audio, user-mode timing of exactly the code the driver runs (the MAC
 * loops are scalar by construction, no vector dispatch: pcm.cpp). Informational; the owner's budget
 * for the alias-free band plan: about twice the taps of the first design, the worst cascade at most
 * ~6 ms per second of stereo audio. Multiplies: a 2:1 output Center + 1 (the symmetric pairs share a
 * tap), a polyphase output Taps; best of 3 runs. */
static void TestRateConverterCost()
{
    for (const RPair &p : kRPairs) {
        const ULONG frames = 4 * p.Engine;   /* 4 s */
        std::vector<LONG> in((size_t)frames * 2);
        RRand r(0xC057u);
        for (LONG &v : in) v = (LONG)r.Next() / 4;
        std::vector<UCHAR> out(((size_t)frames * 2 * 4) / 10 * 11 + 4096);   /* up to 48 / 44.1 of the input */
        const ULONG block = 1024;
        double sec = 1e9;
        ULONG written = 0;
        for (int run = 0; run < 3; ++run) {
            RConv cv(p.Engine, p.Codec);
            auto t0 = std::chrono::steady_clock::now();
            written = 0;
            for (ULONG done = 0; done < frames; done += block)
                written += PnConvertPcmEx(&cv.St, (const UCHAR *)&in[(size_t)done * 2], PN_MIN(block, frames - done) * 8, 2, 32, 32, FALSE,
                                          out.data() + written, (ULONG)out.size() - written, PNA2DP_FMT_S32, 2, 32768);
            sec = PN_MIN(sec, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        }
        double msPerS = sec * 1000.0 / 4.0;
        /* the 2:1 stage makes Codec outputs a second, the polyphase stage Codec * Decimate */
        double macs = (p.Dec ? 2.0 * (p.Dec->Center + 1) * p.Codec : 0) + (p.Rs ? 2.0 * p.Rs->Taps * RMid(p) : 0);
        printf("rate converter %u -> %u Hz cost: %.2f ms per second of stereo audio (%.2f %% of one core), %.1f M MAC/s\n",
               p.Engine, p.Codec, msPerS, msPerS / 10.0, macs / 1e6);
        CHECK(written > 0 && msPerS < 100.0);   /* a sanity bound, 10 % of one core */
    }
}

static void TestLaneR()
{
    TestRateConverterTables();
    TestRateConverterPlan();
    TestRateConverterFraming();
    TestRateConverterModel();
    TestRateConverterAudio();
    TestRateConverterTransition();
    TestRateConverterBlocks();
    TestRateConverterStates();
    TestRateConverterPauseDrain();
    TestRateConverterCost();
}
