"""Runs the end-to-end audio quality harness (quality_test.exe) and summarises results.json.

The harness drives the production PCM conversion, SBC encoder, codec selection, RTP writer
and codec plugins, decodes with FFmpeg and measures SNR, THD+N, level, clipping, clicks and
gaps (see tools/audio-test/quality_test.cpp). This script only runs it and prints a compact
table plus every defect flag. Standard library only. Exit code: 0 clean, 1 defect flags,
2 harness failure. Known issues (a reachable state with a documented, unfixed limit, such as
the dual-channel SBC ABR floor) are listed by name but do not fail the run; the harness fails
it when such a state measures worse than recorded.

Native bit-exactness (0.1.0.22): every job whose Windows rate is the codec rate must convert and
pack exactly as recorded in tools/audio-test/native-baseline-0.1.0.21.json (--hashes-check, on
by default; --no-hashes-check skips it, --hashes-out FILE records a new baseline). The group
"rate" measures the sample-rate converter on its own (rate_* metrics); its transition-band tones and
stepped sweep report the worst alias / image re the tone (trans, tsweep: at most -110 dB, the
alias-free band plan).

  python build/test-audio-quality.py --exe quality_test.exe --plugins out/Release --work DIR

SPDX-License-Identifier: MIT
"""
import argparse
import json
import subprocess
import sys
from pathlib import Path

BASELINE = Path(__file__).resolve().parent.parent / "tools" / "audio-test" / "native-baseline-0.1.0.21.json"


STEADY = {"sine997_0dBFS", "sine997_-1dBFS", "sine997_-6dBFS", "sine997_-20dBFS", "sine997_-60dBFS", "pink_-20dBFSrms",
          "bass60_0dBFS", "L_only", "R_only", "opposite_phase", "L997_R1499", "sweep20-20k_-6dBFS"}


def fmt(v, digits=1):
    if v is None:
        return "-"
    if isinstance(v, float) and abs(v) >= 1000:
        return f"{v:.0f}"
    return f"{v:.{digits}f}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--plugins", required=True)
    ap.add_argument("--work", required=True)
    ap.add_argument("--ffmpeg")
    ap.add_argument("--filter")
    ap.add_argument("--threads", type=int)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--hashes-out")
    ap.add_argument("--no-hashes-check", action="store_true")
    a = ap.parse_args()
    cmd = [str(Path(a.exe).resolve()), "--plugins", str(Path(a.plugins).resolve()), "--work", str(Path(a.work).resolve())]
    if a.ffmpeg:
        cmd += ["--ffmpeg", a.ffmpeg]
    if a.filter:
        cmd += ["--filter", a.filter]
    if a.threads:
        cmd += ["--threads", str(a.threads)]
    if a.keep:
        cmd.append("--keep")
    if a.hashes_out:
        cmd += ["--hashes-out", str(Path(a.hashes_out).resolve())]
    elif not a.no_hashes_check and BASELINE.exists():
        cmd += ["--hashes-check", str(BASELINE)]
    run = subprocess.run(cmd, capture_output=True, text=True)
    lines = run.stdout.splitlines()
    for line in lines:
        if not line.startswith(("FLAG ", "KNOWN ")):
            print(line)
    if run.returncode not in (0, 1):
        print(run.stderr)
        return 2
    results = json.loads((Path(a.work) / "results.json").read_text(encoding="utf-8"))
    jobs = results["jobs"]

    by_cfg = {}
    for j in jobs:
        by_cfg.setdefault(j["config"], []).append(j)

    def pick(cfg, signal, key, fmt_name=None, group=None):
        for j in by_cfg.get(cfg, []):
            if j["signal"] != signal or j["volume_db"] != 0:
                continue
            if group and j["group"] != group:
                continue
            if fmt_name and j["format"] != fmt_name:
                continue
            if not group and j["group"] not in ("codec", "rate"):
                continue
            return j["metrics"].get(key)
        return None

    print()
    print("Per-configuration summary (codec matrix, unity volume):")
    head = (f"{'configuration':34} {'kbps':>5} {'bp':>3} {'THD+N -1':>8} {'THD+N -6':>8} {'THD+N 0':>8} "
            f"{'bass60':>7} {'pink SNR':>8} {'clip0dB':>7} {'clkHF':>6} {'gap ms':>6} {'lvl dB':>6} {'flags':>5}")
    print(head)
    print("-" * len(head))
    for c in results["configs"]:
        key = c["key"]
        js = by_cfg.get(key, [])
        if not js:
            continue
        flags = sum(len(j["flags"]) for j in js)
        # HF residual click score on the steady signals (transient test signals carry legitimate pre-echo)
        clicks = [j["metrics"].get("click_hf_max_db") for j in js
                  if j["group"] in ("codec", "rate") and j["signal"] in STEADY
                  and j["metrics"].get("click_hf_max_db") is not None]
        gaps = [j["metrics"].get("gap_longest_ms") for j in js
                if j["group"] in ("codec", "rate") and j["metrics"].get("gap_longest_ms") is not None]
        lvl = pick(key, "sine997_-6dBFS", "tone_level_err_db")
        clip0 = pick(key, "sine997_0dBFS", "dec_clip_samples")
        first = next((j for j in js), None)
        kbps = first["metrics"].get("bitrate_kbps") if first else None
        bp = first["metrics"].get("sbc_bitpool") if first else None
        thd = lambda s: pick(key, s, "thdn_db")
        print(f"{key:34} {fmt(kbps, 0):>5} {fmt(bp, 0):>3} {fmt(thd('sine997_-1dBFS')):>8} {fmt(thd('sine997_-6dBFS')):>8} "
              f"{fmt(thd('sine997_0dBFS')):>8} {fmt(thd('bass60_0dBFS')):>7} {fmt(pick(key, 'pink_-20dBFSrms', 'snr_db')):>8} "
              f"{fmt(clip0, 0):>7} {fmt(max(clicks) if clicks else None):>6} {fmt(max(gaps) if gaps else None, 2):>6} "
              f"{fmt(lvl, 2):>6} {flags:>5}")
    base = [j for j in jobs if j["group"] == "baseline"]
    if base:
        print()
        print("Independent baseline: FFmpeg's SBC encoder on the same codec input (joint 48 kHz, 345 kbps):")
        for j in base:
            m = j["metrics"]
            print(f"  {j['signal']:24} bitpool {fmt(m.get('sbc_bitpool'), 0)} THD+N {fmt(m.get('thdn_db'))} dB  "
                  f"SNR {fmt(m.get('snr_db'))} dB  clip {fmt(m.get('dec_clip_samples'), 0)}")
    rate_cfgs = [c for c in results["configs"] if c["engine_rate"] != c["rate"]]
    if rate_cfgs:
        print()
        print("Sample-rate converter (group rate; the codec input against the analytic signal):")
        head = (f"  {'configuration':28} {'engine':>7} {'codec':>6} {'delay':>6} {'THD+N24':>8} {'THD+Nf':>7} {'SNR -1':>7} {'sweep':>6} "
                f"{'20k lvl':>8} {'alias':>6} {'image':>6} {'trans':>6} {'tsweep':>6} {'win24':>6} {'flags':>5}")
        print(head)
        for c in rate_cfgs:
            key = c["key"]
            js = [j for j in by_cfg.get(key, []) if j["group"] == "rate"]
            if not js:
                continue

            def rm(signal, metric, fmt_name=None):
                for j in js:
                    if j["signal"] == signal and (fmt_name is None or j["format"] == fmt_name) and metric in j["metrics"]:
                        return j["metrics"][metric]
                return None
            # the worst 1 ms window of the 24-bit / float -1 dBFS tones (the 16-bit and -60 dBFS jobs
            # are bounded by their own noise floor and checked per job)
            wins = [j["metrics"]["rate_window_snr_min_db"] for j in js
                    if "rate_window_snr_min_db" in j["metrics"] and j["format"] in ("s24in32", "f32") and j["signal"] == "sine997_-1dBFS"]
            # the alias-free band plan: the worst first-order alias / image of the transition-band tones
            # and of the stepped sweep (dB re the tone; at most -110 dB)
            trans = [j["metrics"]["rate_transition_alias_db"] for j in js if "rate_transition_alias_db" in j["metrics"]]
            tsweep = [j["metrics"]["rate_sweep_alias_db"] for j in js if "rate_sweep_alias_db" in j["metrics"]]
            alias = [j["metrics"][k] for j in js for k in ("rate_alias_rejection_db",) if k in j["metrics"]]
            image = [j["metrics"][k] for j in js for k in ("rate_image_rejection_db",) if k in j["metrics"]]
            flags = sum(len(j["flags"]) for j in by_cfg.get(key, []))
            print(f"  {key:28} {c['engine_rate']:>7} {c['rate']:>6} {fmt(rm('sine997_-1dBFS', 'rate_delay_ms'), 3):>6} "
                  f"{fmt(rm('sine997_-1dBFS', 'rate_thdn_db', 's24in32')):>8} {fmt(rm('sine997_-1dBFS', 'rate_thdn_db', 'f32')):>7} "
                  f"{fmt(rm('sine997_-1dBFS', 'rate_snr_db')):>7} {fmt(rm('sweep20-20k_-6dBFS', 'rate_snr_db')):>6} "
                  f"{fmt(rm('sine20k_-6dBFS', 'rate_level_db'), 4):>8} {fmt(min(alias) if alias else None):>6} {fmt(min(image) if image else None):>6} "
                  f"{fmt(max(trans) if trans else None):>6} {fmt(max(tsweep) if tsweep else None):>6} {fmt(min(wins) if wins else None):>6} {flags:>5}")
    prof = [j for j in jobs if j["group"].startswith("profile-") or j["group"] == "fault-trim"]
    if prof:
        print()
        print("Latency profiles (radio stall; latency trims at the profile's target / max):")
        for j in prof:
            m = j["metrics"]
            t, mx = m.get("latency_target_ms", 60), m.get("latency_max_ms", 250)
            print(f"  {j['id']:78} {fmt(t, 0):>4} / {fmt(mx, 0):>4} ms  trims {fmt(m.get('trims', 0), 0)}  flags {len(j['flags'])}")
    print()
    known = [j for j in jobs if j.get("known")]
    if known:
        print(f"{len(known)} job(s) measure a known issue (reported, not failures; bounded by the harness):")
        for j in known:
            for f in j["known"]:
                print(f"  {j['id']}: {f}")
        print()
    flagged = [j for j in jobs if j["flags"]]
    glob = results.get("global_flags", [])
    print(f"{len(flagged)} of {len(jobs)} jobs carry defect flags; {len(glob)} global flag(s):")
    for f in glob:
        print(f"  global: {f}")
    for j in flagged:
        for f in j["flags"]:
            print(f"  {j['id']}: {f}")
    return 1 if flagged or glob else 0


if __name__ == "__main__":
    sys.exit(main())
