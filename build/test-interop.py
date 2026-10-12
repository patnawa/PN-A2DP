"""Verify exported SBC vectors against an independent FFmpeg decoder (stdlib only)."""
import array
import math
from pathlib import Path
import subprocess
import sys


def samples(path):
    result = array.array("h")
    result.frombytes(path.read_bytes())
    if sys.byteorder != "little":
        result.byteswap()
    return result


def main():
    root = Path(sys.argv[1])
    vectors = sorted(root.glob("*.sbc"))
    if not vectors:
        raise RuntimeError("No SBC interoperability vectors found")
    failures = 0
    for vector in vectors:
        decoded = vector.with_suffix(".sbc.decoded.pcm")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "sbc", "-i", str(vector),
                        "-f", "s16le", str(decoded)], check=True)
        source, actual = samples(vector.with_suffix(".pcm")), samples(decoded)
        channels = 1 if vector.stem.split("-")[1] == "0" else 2
        frames = min(len(source), len(actual)) // channels
        begin, end = 512, min(frames - 256, 4512)
        delay = min(range(200), key=lambda d: sum((source[i * channels] - actual[(i + d) * channels]) ** 2
                                                  for i in range(begin, end)))
        signal = sum(x * x for x in source[begin * channels:end * channels])
        error = sum((source[i * channels + c] - actual[(i + delay) * channels + c]) ** 2
                    for i in range(begin, end) for c in range(channels))
        snr = 10 * math.log10(signal / max(error, 1))
        threshold = 45 if "-p" in vector.stem and not vector.stem.endswith("-p0") else 20
        ok = len(source) == len(actual) and snr > threshold
        print(f"{'PASS' if ok else 'FAIL'} {vector.name}: {snr:.2f} dB, delay {delay}, {frames} frames")
        failures += not ok
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
