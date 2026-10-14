"""Generates driver/pna2dp/inc/resample_coeffs.h, the fixed-point coefficient tables of the
kernel sample-rate converter (driver/pna2dp/src/pcm.cpp). Standard library only, deterministic.

  python build/gen-resampler-coeffs.py            # rewrite the header
  python build/gen-resampler-coeffs.py --check    # exit 1 when the committed header differs

The kernel has no floating point, so the tables are designed here in double precision and
committed as integers; the generator then measures exactly these integers and refuses to write a
header that misses the specification. core_test measures them again (driver/pna2dp/test/
core_test_R.inl, TestRateConverterTables) and runs the converter itself on tones.

Specification: the alias-free band plan (owner decision, 14 October 2026; docs/20-max-audio-
quality.md, "#1"). Every conversion passes 0..20 kHz with a ripple of at most 0.01 dB and stops
from the LOWER of its input and output Nyquist frequencies (22.05 kHz whenever 44.1 kHz is
involved, 24 kHz for 96 / 88.2 -> 48 kHz) by at least 110 dB (design target 120 dB): no alias or image of
any input frequency comes out above -110 dB. The cascades run the polyphase stage first, at the
engine rate, into the other family's double rate fmid, then the 2:1 stage (88.2 -> 96 -> 48 kHz
and 96 -> 88.2 -> 44.1 kHz; review of 0.1.0.22, the kernel's CPU budget). The 2:1 stage protects
the final band on its own (its stopband starts at the final Nyquist frequency), so the polyphase
stage only stops what would fold onto 0 .. fout / 2 at fmid: from fmid - fout / 2 = 0.75 fmid
(72 kHz into 96 kHz, 66.15 kHz into 88.2 kHz); everything between its passband and that edge lands
in the 2:1 stage's stopband. Its short prototype (14 / 17 taps per phase) replaces the first
design's 2:1 FIR at 88.2 kHz plus 173 taps per output (88.2 -> 48 kHz), and the 2:1 FIR at 96 kHz
plus 188 taps per output (96 -> 44.1 kHz).

Design:
  - Kaiser-windowed sinc, design attenuation A = 123 dB (beta = 0.1102 (A - 8.7) = 12.5959),
    cutoff at the centre of the transition band, length from the Kaiser estimate
    (A - 7.95) / (14.36 df) + 1. A = 123 dB puts the first stopband sidelobe of every quantised
    table at least 121.9 dB down (A = 120 dB left the stopband edge at -119.5 dB).
  - Polyphase L/M = 160/147 (44.1 -> 48 kHz) and 147/160 (48 -> 44.1 kHz), prototypes at
    F = Fin L = 7.056 MHz, passband 20 kHz, stopband 22.05 kHz, cutoff 21.025 kHz; K taps per
    phase = ceil(estimate / L), length N = K L, less one when even (odd: linear phase about
    (N - 1) / 2). Taps are stored oldest input first: Coef[p][j] multiplies the j-th oldest of
    the K inputs of phase p's window.
  - The cascades' polyphase stage 1, by the same rule: 160/147 from 88.2 kHz into 96 kHz and
    147/160 from 96 kHz into 88.2 kHz, prototypes at F = 14.112 MHz, passband 20 kHz, stopband
    from 72 kHz and 66.15 kHz (cutoff 46 and 43.075 kHz).
  - 2:1 decimation FIRs, odd length, symmetric: 96 -> 48 kHz passband 20 kHz, stopband 24 kHz
    (also stage 2 of 88.2 -> 48 kHz); 88.2 -> 44.1 kHz passband 20 kHz, stopband 22.05 kHz (also
    stage 2 of 96 -> 44.1 kHz). Stored from the centre: Coef[0] is the centre tap, Coef[k] the
    pair at centre +- k.
  - Quantisation, Q28, rounded half away from zero: every polyphase phase sums to exactly 2^28
    (the residual on its largest tap, lowest index on ties); a 2:1 FIR sums to exactly 2^28 (the
    residual on its centre tap, so it stays exactly symmetric). DC and digital silence are exact,
    no phase has its own gain.

Checks, on the shipped integers (the generator fails when one does not hold):
  - passband ripple from 0 to 20 kHz at most 0.0001 dB (the FFT grid and the exact edge);
  - a dense scan of the WHOLE stopband, from its edge to half the filter's rate: an FFT of the
    zero-padded prototype (2^20 points, 6.7 Hz apart at 7.056 MHz; 2^16 for the 2:1 FIRs), the
    eight largest peaks refined by golden-section search: at least 120 dB down;
  - an explicit alias / image check of the six conversions as the converter runs them (cascades:
    the polyphase stage, then the 2:1 stage): for input tones every 10 Hz from 19 kHz to the input's Nyquist
    frequency and every 100 Hz below, every output component except the tone itself, computed
    from the integers by the polyphase identity H(f + k Fin) = sum_p A_p(f) e^(-j 2 pi k p / L):
    at most -110 dB re the tone (reported; an unwanted component already below the worst one
    found is not expanded through stage 2, which cannot raise it); and each conversion's
    passband ripple (the tone's own level from 100 Hz to 20 kHz) at most 0.0001 dB;
  - overflow: every phase's and every 2:1 FIR's sum of |taps| at most 3.5 * 2^28. Inputs of up to
    2^33 (S32 units, the +-4.0 float limit) then keep the 64-bit accumulator below 2^63 (any sum
    below 4 would), and pcm.cpp clamps stage 1's output to +-2^33 before stage 2.

SPDX-License-Identifier: MIT
"""
import argparse
import cmath
import math
import operator
import sys
from pathlib import Path

GEN_VERSION = 4
ATTENUATION_DB = 123.0
BETA = 0.1102 * (ATTENUATION_DB - 8.7)
# The cascades' polyphase stage 1 is short (its transition band is wide), so it is designed far
# deeper for a few more taps: its stopband holds the images of the whole passband (k Fin -+ f),
# which fold into the final band; at 123 dB they summed to a THD+N of -128 dB re a 997 Hz tone.
ATTENUATION_STAGE1_DB = 150.0
Q = 28
ONE = 1 << Q
PASSBAND_HZ = 20000.0
RIPPLE_MAX_DB = 0.0001     # asserted (specification 0.01 dB)
STOP_MIN_DB = 120.0        # asserted (specification 110 dB, design target 120 dB)
ALIAS_MAX_DB = -110.0      # asserted: every alias / image of a tone, re the tone
MAX_ABS_SUM = 3.5          # per phase / per 2:1 FIR, times 2^28

ROOT = Path(__file__).resolve().parent.parent
HEADER = ROOT / "driver" / "pna2dp" / "inc" / "resample_coeffs.h"


def i0(x):
    """Modified Bessel function of the first kind, order 0 (power series)."""
    s = t = 1.0
    k = 1
    while True:
        t *= (x / (2.0 * k)) ** 2
        s += t
        if t < 1e-17 * s:
            return s
        k += 1


def round_half_away(x):
    return int(math.floor(abs(x) + 0.5)) * (1 if x >= 0 else -1)


def kaiser_estimate(df, atten=ATTENUATION_DB):
    """Kaiser's length estimate for transition width df (cycles per sample)."""
    return (atten - 7.95) / (14.36 * df) + 1.0


def kaiser_sinc(N, fc, gain, atten=ATTENUATION_DB):
    """N taps (odd), cutoff fc in cycles per sample, centre (N - 1) / 2, DC gain about `gain`."""
    centre = (N - 1) // 2
    beta = 0.1102 * (atten - 8.7)
    i0beta = i0(beta)
    h = []
    for n in range(N):
        x = n - centre
        s = 2.0 * fc if x == 0 else math.sin(2.0 * math.pi * fc * x) / (math.pi * x)
        r = x / centre
        h.append(gain * s * i0(beta * math.sqrt(max(0.0, 1.0 - r * r))) / i0beta)
    return h


def design_poly(fin, fout, L, M, fstop=None, atten=ATTENUATION_DB):
    """Polyphase stage L/M: prototype at F = fin L and its quantised table (oldest input first).
    The stopband starts at fstop: the lower Nyquist frequency for a conversion on its own; in a
    cascade (a 2:1 stage follows) where the stage's output would fold onto the final band."""
    F = fin * L
    if fstop is None:
        fstop = min(fin, fout) / 2.0
    fc = (PASSBAND_HZ + fstop) / 2.0 / F
    K = math.ceil(kaiser_estimate((fstop - PASSBAND_HZ) / F, atten) / L)
    N = K * L
    if N % 2 == 0:
        N -= 1
    h = kaiser_sinc(N, fc, L, atten)
    table, worst_abs = [], 0
    for p in range(L):
        taps = [h[p + L * k] if p + L * k < N else 0.0 for k in range(K)]   # newest input first
        total = sum(taps)
        q = [round_half_away(t / total * ONE) for t in taps]
        big = max(range(K), key=lambda k: (abs(q[k]), -k))
        q[big] += ONE - sum(q)
        assert sum(q) == ONE
        abs_sum = sum(abs(v) for v in q)
        assert abs_sum <= MAX_ABS_SUM * ONE, (L, M, p, abs_sum / ONE)
        worst_abs = max(worst_abs, abs_sum)
        table.append(q[::-1])          # oldest input first
    proto = [0.0] * N                  # the shipped prototype, newest-first index n = p + L k
    for p in range(L):
        row = table[p][::-1]
        for k in range(K):
            if p + L * k < N:
                proto[p + L * k] = row[k] / ONE
            else:
                assert row[k] == 0
    return {"kind": "poly", "fin": fin, "fout": fout, "L": L, "M": M, "K": K, "N": N, "F": F, "fc": fc * F, "atten": atten,
            "fstop": fstop, "table": table, "proto": proto, "gain": float(L), "abs_sum": worst_abs / ONE}


def design_dec2(fin, fstop):
    """2:1 decimation FIR at fin: odd length, symmetric, Coef[0] the centre, Coef[k] at centre +- k."""
    fc = (PASSBAND_HZ + fstop) / 2.0 / fin
    N = math.ceil(kaiser_estimate((fstop - PASSBAND_HZ) / fin))
    if N % 2 == 0:
        N += 1
    centre = (N - 1) // 2
    h = kaiser_sinc(N, fc, 1.0)
    total = sum(h)
    side = [round_half_away(h[centre + k] / total * ONE) for k in range(1, centre + 1)]
    coef = [ONE - 2 * sum(side)] + side
    proto = [0.0] * N
    proto[centre] = coef[0] / ONE
    for k in range(1, centre + 1):
        proto[centre - k] = proto[centre + k] = coef[k] / ONE
    abs_sum = abs(coef[0]) + 2 * sum(abs(v) for v in side)
    assert coef[0] + 2 * sum(side) == ONE and abs_sum <= MAX_ABS_SUM * ONE, (fin, abs_sum / ONE)
    return {"kind": "dec2", "fin": fin, "fout": fin / 2.0, "N": N, "centre": centre, "F": fin, "fc": fc * fin,
            "fstop": fstop, "coef": coef, "proto": proto, "gain": 1.0, "abs_sum": abs_sum / ONE}


# ---- the dense response scan --------------------------------------------------------------------

def fft(x):
    """Radix-2 FFT (decimation in time) of a list of complex numbers, length a power of 2."""
    n = len(x)
    levels = n.bit_length() - 1
    rev = [0] * n
    for i in range(1, n):
        rev[i] = (rev[i >> 1] >> 1) | ((i & 1) << (levels - 1))
    a = [x[r] for r in rev]
    twiddle = [cmath.exp(-2j * math.pi * k / n) for k in range(n // 2)]
    size = 2
    while size <= n:
        half = size // 2
        step = n // size
        if half < 64:                  # few twiddles: one strided pass per twiddle
            for j in range(half):
                w = twiddle[j * step]
                lo = a[j::size]
                t = [w * v for v in a[j + half::size]]
                a[j::size] = list(map(operator.add, lo, t))
                a[j + half::size] = list(map(operator.sub, lo, t))
        else:                          # long blocks: one pass per block
            tw = twiddle[::step][:half]
            for start in range(0, n, size):
                lo = a[start:start + half]
                t = list(map(operator.mul, a[start + half:start + size], tw))
                a[start:start + half] = map(operator.add, lo, t)
                a[start + half:start + size] = map(operator.sub, lo, t)
        size *= 2
    return a


def gain_db(h, f, rate, gain):
    """|H(f)| / gain in dB, f in Hz at `rate` (rotation recurrence over the taps)."""
    z = cmath.exp(-2j * math.pi * f / rate)
    w, acc = 1.0 + 0j, 0j
    for v in h:
        acc += v * w
        w *= z
    return 20.0 * math.log10(max(abs(acc) / gain, 1e-300))


def response(d, points):
    """Passband ripple to 20 kHz and the worst stopband level from the stopband edge to rate / 2:
    the FFT grid of the zero-padded prototype, its eight largest stopband peaks refined by
    golden-section search, and the exact band edges."""
    h, rate, gain = d["proto"], d["F"], d["gain"]
    X = fft([complex(v) for v in h] + [0j] * (points - len(h)))
    df = rate / points
    db = [20.0 * math.log10(max(abs(X[k]) / gain, 1e-300)) for k in range(points // 2 + 1)]
    kp = int(PASSBAND_HZ / df)
    edge = gain_db(h, PASSBAND_HZ, rate, gain)
    pb = db[:kp + 1] + [edge]
    ripple = max(pb) - min(pb)
    ks = int(math.ceil(d["fstop"] / df))
    peaks = [k for k in range(ks, points // 2 + 1)
             if db[k] >= (db[k - 1] if k > ks else -1e9) and db[k] >= (db[k + 1] if k < points // 2 else -1e9)]
    peaks.sort(key=lambda k: -db[k])
    worst, worst_at = gain_db(h, d["fstop"], rate, gain), d["fstop"]
    golden = (math.sqrt(5.0) - 1.0) / 2.0
    for k in peaks[:8]:
        a, b = max(d["fstop"], (k - 1) * df), min(rate / 2.0, (k + 1) * df)
        c1, c2 = b - golden * (b - a), a + golden * (b - a)
        g1, g2 = gain_db(h, c1, rate, gain), gain_db(h, c2, rate, gain)
        for _ in range(24):
            if g1 > g2:
                b, c2, g2 = c2, c1, g1
                c1 = b - golden * (b - a)
                g1 = gain_db(h, c1, rate, gain)
            else:
                a, c1, g1 = c1, c2, g2
                c2 = a + golden * (b - a)
                g2 = gain_db(h, c2, rate, gain)
        for g, f in ((db[k], k * df), (g1, c1), (g2, c2)):
            if g > worst:
                worst, worst_at = g, f
    assert ripple <= RIPPLE_MAX_DB and -worst >= STOP_MIN_DB, (d["kind"], d["fin"], ripple, worst, worst_at)
    return ripple, worst, worst_at


# ---- the alias / image check ----------------------------------------------------------------------

def fold(f, rate):
    """Where a tone of f Hz lands after sampling at rate: folded into 0 .. rate / 2."""
    f = math.fmod(f, rate)
    if f < 0:
        f += rate
    return rate - f if f > rate / 2.0 else f


class Stage:
    """One converter stage on the shipped integers: the real output components of a real input tone."""

    def __init__(self, d):
        self.d = d
        if d["kind"] == "poly":
            L, K, h = d["L"], d["K"], d["proto"]
            self.phases = [[h[p + L * k] if p + L * k < len(h) else 0.0 for k in range(K)] for p in range(L)]
            self.dft = [[cmath.exp(-2j * math.pi * k * p / L) for p in range(L)] for k in range(L)]
        else:
            self.side = [c / ONE for c in d["coef"]]

    def components(self, f):
        """[(output frequency, amplitude, k)] for a tone of f Hz and unit amplitude at the stage's
        input; k = 0 is the tone's own path (the image / alias index otherwise)."""
        d = self.d
        if d["kind"] == "dec2":
            w = 2.0 * math.pi * f / d["fin"]
            a = self.side[0] + 2.0 * sum(c * math.cos(w * k) for k, c in enumerate(self.side) if k)
            return [(fold(f, d["fout"]), abs(a), 0)]
        L, K, fin, F = d["L"], d["K"], d["fin"], d["F"]
        zm = [cmath.exp(-2j * math.pi * f * m / fin) for m in range(K)]
        rp, r = 1.0 + 0j, cmath.exp(-2j * math.pi * f / F)
        A = []
        for row in self.phases:
            A.append(rp * sum(map(operator.mul, row, zm)))
            rp *= r
        out = []
        for k in range(L):
            Hk = sum(map(operator.mul, self.dft[k], A))
            out.append((fold(f + k * fin, d["fout"]), abs(Hk) / L, k))
        return out


def alias_check(name, stages):
    """The worst output component other than the tone itself, over input tones every 100 Hz below
    19 kHz and every 10 Hz from 19 kHz to the input's Nyquist frequency (exclusive), and the
    conversion's passband ripple (the tone's own level from 100 Hz to 20 kHz). A stage passes no
    component more than 1.0001 times its own amplitude (the passband ripple is far below
    0.0001 dB), so an unwanted component already below the worst found is not expanded further."""
    fin = stages[0].d["fin"]
    tones = [100.0 * i for i in range(1, 190)] + [19000.0 + 10.0 * i for i in range(int((fin / 2.0 - 19000.0) / 10.0))]
    tones = [t for t in tones if t < fin / 2.0]
    worst, worst_tone, worst_at = 0.0, 0.0, 0.0
    own_lo, own_hi = 1e9, -1e9
    for f in tones:
        comps = [(f, 1.0, True)]                     # (frequency, amplitude, the tone's own path)
        for st in stages:
            nxt = []
            for g, a, own in comps:
                if not own and a * 1.0001 <= worst:
                    continue
                for g2, a2, k in st.components(g):
                    nxt.append((g2, a * a2, own and k == 0 and g < st.d["fout"] / 2.0))
            comps = nxt
        for g, a, own in comps:
            if not own and a > worst:
                worst, worst_tone, worst_at = a, f, g
            if own and f <= PASSBAND_HZ:
                db = 20.0 * math.log10(a)
                own_lo, own_hi = min(own_lo, db), max(own_hi, db)
    level = 20.0 * math.log10(max(worst, 1e-300))
    ripple = own_hi - own_lo
    assert level <= ALIAS_MAX_DB and ripple <= RIPPLE_MAX_DB, (name, level, worst_tone, worst_at, ripple)
    return level, worst_tone, worst_at, ripple


# ---- the header ---------------------------------------------------------------------------------

def rows(values, per_line, indent):
    out = []
    for i in range(0, len(values), per_line):
        out.append(indent + ", ".join(str(v) for v in values[i:i + per_line]))
    return ",\n".join(out)


def render(d96, d88, up, down, up2, down2, checks):
    lines = [
        "/* resample_coeffs.h - GENERATED by build/gen-resampler-coeffs.py (version %d). Do not edit:" % GEN_VERSION,
        " * change the generator and run it again (--check verifies this file is current).",
        " *",
        " * Fixed-point (Q28) tables of the kernel sample-rate converter. pcm.cpp includes this file",
        " * exactly once; pcm.h declares the tables and their sizes and pcm.cpp asserts they agree.",
        " * Alias-free band plan: passband 0..20 kHz, stopband from the lower of the input and output",
        " * Nyquist frequencies (22.05 kHz with 44.1 kHz involved, 24 kHz into 48 kHz from 96 / 88.2). The",
        " * cascades run a short polyphase stage first (88.2 -> 96 kHz and 96 -> 88.2 kHz, stopband",
        " * from 0.75 of its output rate: what would fold onto the final band), then the 2:1 FIR.",
        " * Kaiser-windowed sinc, A = %.0f dB (beta %.4f), cutoff at the transition band's centre, linear"
        % (ATTENUATION_DB, BETA),
        " * phase. Each polyphase phase sums to exactly 2^28 (the rounding residual sits on its largest",
        " * tap), taps oldest input first; a 2:1 FIR sums to exactly 2^28 (the residual on its centre),",
        " * stored from the centre. Measured by the generator on these integers (core_test measures",
        " * them again and runs the converter):",
    ]
    for c in checks:
        lines.append(" *   " + c)
    lines += [
        " * SPDX-License-Identifier: MIT",
        " */",
        "#pragma once",
        "",
        "#define PN_RS_GEN_VERSION              %d" % GEN_VERSION,
        "#define PN_RS_DEC96_TAPS               %d      /* 96 -> 48 kHz 2:1 FIR (odd, symmetric) */" % d96["N"],
        "#define PN_RS_DEC88_TAPS               %d      /* 88.2 -> 44.1 kHz 2:1 FIR */" % d88["N"],
        "#define PN_RS_160X147_TAPS             %d      /* 44.1 -> 48 kHz: taps per phase */" % up["K"],
        "#define PN_RS_160X147_CENTER_TWICE     %d   /* N - 1 of the %d-tap prototype at 7.056 MHz */" % (up["N"] - 1, up["N"]),
        "#define PN_RS_147X160_TAPS             %d      /* 48 -> 44.1 kHz */" % down["K"],
        "#define PN_RS_147X160_CENTER_TWICE     %d   /* N - 1 of the %d-tap prototype at 7.056 MHz */" % (down["N"] - 1, down["N"]),
        "#define PN_RS_88X96_TAPS               %d       /* 88.2 -> 96 kHz (160/147), stage 1 of 88.2 -> 48 kHz */" % up2["K"],
        "#define PN_RS_88X96_CENTER_TWICE       %d    /* N - 1 of the %d-tap prototype at 14.112 MHz */" % (up2["N"] - 1, up2["N"]),
        "#define PN_RS_96X88_TAPS               %d       /* 96 -> 88.2 kHz (147/160), stage 1 of 96 -> 44.1 kHz */" % down2["K"],
        "#define PN_RS_96X88_CENTER_TWICE       %d    /* N - 1 of the %d-tap prototype at 14.112 MHz */" % (down2["N"] - 1, down2["N"]),
        "",
    ]
    for name, d, what in (("g_PnDec96Q28", d96, "96 -> 48 kHz"), ("g_PnDec88Q28", d88, "88.2 -> 44.1 kHz")):
        lines.append("/* 2:1 %s, %d taps: [0] the centre tap, [k] the taps at centre +- k */" % (what, d["N"]))
        lines.append("const LONG %s[%d] = {" % (name, d["centre"] + 1))
        lines.append(rows(d["coef"], 9, "    "))
        lines.append("};")
        lines.append("")
    tables = (("g_PnRs160x147Q28", up, "44.1 -> 48 kHz (L 160, M 147)"), ("g_PnRs147x160Q28", down, "48 -> 44.1 kHz (L 147, M 160)"),
              ("g_PnRs88to96Q28", up2, "88.2 -> 96 kHz (L 160, M 147), stage 1 of 88.2 -> 48 kHz"),
              ("g_PnRs96to88Q28", down2, "96 -> 88.2 kHz (L 147, M 160), stage 1 of 96 -> 44.1 kHz"))
    for i, (name, d, what) in enumerate(tables):
        if i:
            lines.append("")
        lines.append("/* %s: [phase][tap], oldest input first */" % what)
        lines.append("const LONG %s[%d][%d] = {" % (name, d["L"], d["K"]))
        body = []
        for p, row in enumerate(d["table"]):
            body.append("    { /* phase %d */\n%s }" % (p, rows(row, 12, "      ")))
        lines.append(",\n".join(body))
        lines.append("};")
    return ("\n".join(lines) + "\n").replace("\n", "\r\n")


def khz(f):
    return ("%.3f" % (f / 1000.0)).rstrip("0").rstrip(".")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=str(HEADER))
    ap.add_argument("--check", action="store_true", help="compare with the existing file instead of writing it")
    a = ap.parse_args()
    d96 = design_dec2(96000.0, 24000.0)
    d88 = design_dec2(88200.0, 22050.0)
    up = design_poly(44100.0, 48000.0, 160, 147)
    down = design_poly(48000.0, 44100.0, 147, 160)
    up2 = design_poly(88200.0, 96000.0, 160, 147, fstop=0.75 * 96000.0, atten=ATTENUATION_STAGE1_DB)
    down2 = design_poly(96000.0, 88200.0, 147, 160, fstop=0.75 * 88200.0, atten=ATTENUATION_STAGE1_DB)
    checks = []
    for d, what in ((d96, "2:1 96 -> 48 kHz"), (d88, "2:1 88.2 -> 44.1 kHz")):
        r, s, at = response(d, 1 << 16)
        checks.append("%s: %d taps (centre %d), ripple %.6f dB to 20 kHz, stopband %.1f dB from %s kHz (worst at %s kHz), sum |h| %.3f"
                      % (what, d["N"], d["centre"], r, -s, khz(d["fstop"]), khz(at), d["abs_sum"]))
    for d in (up, down, up2, down2):
        r, s, at = response(d, 1 << 20)
        checks.append("%d/%d from %s kHz: N %d, %d taps per phase, ripple %.6f dB to 20 kHz, stopband %.1f dB from %s kHz (worst at %s kHz), "
                      "max phase sum |h| %.3f" % (d["L"], d["M"], khz(d["fin"]), d["N"], d["K"], r, -s, khz(d["fstop"]), khz(at), d["abs_sum"]))
    s96, s88, sup, sdown, sup2, sdown2 = Stage(d96), Stage(d88), Stage(up), Stage(down), Stage(up2), Stage(down2)
    conversions = (("44.1 -> 48", [sup]), ("48 -> 44.1", [sdown]), ("96 -> 48", [s96]), ("88.2 -> 44.1", [s88]),
                   ("88.2 -> 48", [sup2, s96]), ("96 -> 44.1", [sdown2, s88]))
    checks.append("conversions as the converter runs them, input tones every 10 Hz from 19 kHz to the input's Nyquist")
    checks.append("  frequency and every 100 Hz below: the worst alias / image (every output component but the tone")
    checks.append("  itself, dB re the tone) and the passband ripple (the tone's own level, 100 Hz to 20 kHz):")
    for name, stages in conversions:
        level, tone, at, ripple = alias_check(name, stages)
        checks.append("  %s kHz: %.1f dB (%s kHz tone at %s kHz), ripple %.6f dB" % (name, level, khz(tone), khz(at), ripple))
    text = render(d96, d88, up, down, up2, down2, checks)
    out = Path(a.out)
    if a.check:
        current = out.read_bytes().decode("ascii") if out.exists() else ""
        if current != text:
            print(f"{out}: differs from the generator output (run build/gen-resampler-coeffs.py)")
            return 1
        print(f"{out}: current")
        return 0
    out.write_bytes(text.encode("ascii"))
    for c in checks:
        print(c)
    print(f"wrote {out} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
