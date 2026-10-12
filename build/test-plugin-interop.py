"""Decode exported AAC/aptX plugin output with FFmpeg. No Bluetooth device needed.

LDAC is covered by the encoder matrix; this script does not claim LDAC decoding.
Uses only Python's standard library. SPDX-License-Identifier: MIT
"""
import array
import json
import math
from pathlib import Path
import subprocess
import sys


def samples(path):
    data = array.array("f")
    data.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        data.byteswap()
    return data


def check(path):
    codec, rate, channels, fmt, mtu, quality = map(int, path.stem.split("-"))
    demux = {2: "loas", 3: "aptx", 4: "aptx_hd", 5: "aptx"}[codec]
    output = path.with_suffix(".decoded.f32")
    args = ["ffmpeg", "-v", "error", "-y", "-f", demux]
    if demux != "loas":
        args += ["-sample_rate", str(rate)]
    subprocess.run(args + ["-i", str(path), "-f", "f32le", str(output)], check=True)
    source, actual = samples(path.with_suffix(".f32")), samples(output)
    if not actual or not all(math.isfinite(x) for x in actual):
        raise RuntimeError(f"Invalid decoded samples: {path.name}")
    frames = min(len(source), len(actual)) // channels
    maximum = 4096 if codec == 2 else 256
    end = frames - maximum - 512
    # The combined bass/transient/stereo signal identifies the delay uniquely.
    # Coarse/fine search keeps this bounded without a NumPy dependency.
    indices = list(range(1024, end, 31))
    def error(delay):
        return sum((source[i * channels] - actual[(i + delay) * channels]) ** 2 for i in indices)
    coarse = min(range(0, maximum + 1, 8), key=error)
    delay = min(range(max(0, coarse - 8), min(maximum, coarse + 8) + 1), key=error)
    sections = []
    for part in range(4):
        begin = max(1024, part * rate // 4 + 512)
        finish = min(frames - delay - 512, (part + 1) * rate // 4 - 512)
        a = [source[i * channels + c] for i in range(begin, finish) for c in range(channels)]
        b = [actual[(i + delay) * channels + c] for i in range(begin, finish) for c in range(channels)]
        if not a:
            raise RuntimeError("Insufficient decoded audio for quality assessment")
        energy = sum(x * x for x in a)
        noise = sum((x - y) ** 2 for x, y in zip(a, b))
        snr = 10 * math.log10(energy / max(noise, 1e-30))
        gain_db = 10 * math.log10(max(sum(x * x for x in b), 1e-30) / energy)
        sections.append({"snr_db": round(snr, 2), "gain_db": round(gain_db, 3)})
    # Lossy AAC can ring at a full-scale discontinuity. This tests gross
    # corruption, channel/polarity errors and attenuation, not losslessness.
    passed = all(s["snr_db"] > 20 and abs(s["gain_db"]) < 0.75 for s in sections)
    result = {"file": path.name, "passed": passed, "delay": delay, "sections": sections}
    print(f"{'PASS' if passed else 'FAIL'} {path.name}: delay {delay}, " +
          ", ".join(f'{s["snr_db"]:.1f} dB SNR/{s["gain_db"]:+.3f} dB gain' for s in sections))
    return result


def main():
    directory = Path(sys.argv[1])
    vectors = sorted(directory.glob("*.bin"))
    if not vectors:
        raise RuntimeError("No plugin vectors found")
    results = [check(path) for path in vectors]
    (directory / "quality-results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
    return int(any(not r["passed"] for r in results))


if __name__ == "__main__":
    sys.exit(main())
