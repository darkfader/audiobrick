#!/usr/bin/env python3
"""Cut a seamlessly looping clip out of a longer recording of a steady sound (rain, a fan, wind, a crowd ...).

  python tools/make_loop.py source.ogg clips/rain_window_loop.mp3 [--min 12] [--max 30] [--cross 1.5]

How it works:
  1. Measures loudness and tone (1/3-octave bands) of the recording every 0.25 s.
  2. Looks for the two moments, between --min and --max seconds apart, that sound most alike.
  3. Cuts between them and joins the end to the start with an equal-power crossfade (no loudness dip for
     unrelated sounds, unlike a straight linear fade), so the last sample flows into the first one.
  4. Normalises the loudness to -18 LUFS (peaks under -2 dBFS) and writes a 112 kbps mono MP3.
The result is then run through tools/check_loop.py; the exit status is 0 only if the loop passes. Only clips that pass
should be called *_loop.mp3.
Needs ffmpeg on the PATH and numpy.
"""
import os
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(__file__))
import check_loop  # noqa: E402

FS = 48000
HOP = 0.25
WIN = 1.0


def decode(path):
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(FS), "pipe:1"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4").copy()


def features(x):
    n, h = int(WIN * FS), int(HOP * FS)
    feats, levels = [], []
    for i in range(0, len(x) - n, h):
        seg = x[i:i + n]
        levels.append(check_loop.rms_db(seg))
        b = check_loop.band_levels(seg)
        feats.append(b - b.mean())
    return np.array(levels), np.array(feats)


def best_pair(levels, feats, lo_s, hi_s, last_j):
    k0, k1 = int(lo_s / HOP), int(hi_s / HOP)
    best = None
    n = len(levels)
    for i in range(n):
        for j in range(i + k0, min(n, i + k1 + 1, last_j + 1)):
            cost = abs(levels[i] - levels[j]) / 2.0 + float(np.mean(np.abs(feats[i] - feats[j]))) / 3.0
            # also compare the neighbourhood so that the content flowing in and out is alike
            if i > 0 and j + 1 < n:
                cost += 0.5 * (abs(levels[i - 1] - levels[j - 1]) / 2.0 + abs(levels[i + 1] - levels[min(j + 1, n - 1)]) / 2.0)
            if best is None or cost < best[0]:
                best = (cost, i, j)
    return best


def build(x, i, j, cross):
    a, b = int(i * HOP * FS), int(j * HOP * FS)
    xs = int(cross * FS)
    # nudge the join to the nearest upward zero crossing so tonal hum keeps its phase
    def zc(p):
        for d in range(0, 400):
            for q in (p + d, p - d):
                if 0 < q < len(x) - 1 and x[q - 1] <= 0 < x[q]:
                    return q
        return p
    a, b = zc(a), zc(b)
    L = b - a
    if L <= 2 * xs or b + xs > len(x):
        raise SystemExit("segment too short for the crossfade")
    t = np.linspace(0, 1, xs, endpoint=False)
    fade_in, fade_out = np.sin(0.5 * np.pi * t), np.cos(0.5 * np.pi * t)   # equal power
    head = x[a:a + xs] * fade_in + x[b:b + xs] * fade_out
    return np.concatenate([head, x[a + xs:a + L]])


def main():
    argv = sys.argv[1:]
    opt, args = {}, []
    k = 0
    while k < len(argv):
        if argv[k] in ("--min", "--max", "--cross") and k + 1 < len(argv):
            opt[argv[k]] = argv[k + 1]
            k += 2
        else:
            args.append(argv[k])
            k += 1
    if len(args) != 2:
        sys.exit(__doc__)
    src, dst = args
    lo, hi, cross = float(opt.get("--min", 12)), float(opt.get("--max", 30)), float(opt.get("--cross", 1.5))
    x = decode(src)
    print(f"source: {len(x) / FS:.1f} s")
    levels, feats = features(x)
    last_j = int((len(x) / FS - cross - 0.5) / HOP)   # the crossfade reads `cross` seconds past the cut
    cost, i, j = best_pair(levels, feats, lo, hi, last_j)
    print(f"best match: {i * HOP:.2f} s and {j * HOP:.2f} s -> loop of {(j - i) * HOP:.1f} s (match cost {cost:.2f}, lower is better)")
    loop = build(x, i, j, cross)
    tmp = os.path.join(tempfile.gettempdir(), "make_loop_tmp.wav")
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-f", "f32le", "-ar", str(FS), "-ac", "1", "-i", "pipe:0", tmp],
                   input=loop.astype("<f4").tobytes(), check=True)
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", tmp, "-af", "loudnorm=I=-18:TP=-2:LRA=7,aresample=48000",
                    "-ac", "1", "-b:a", "112k", dst], check=True)
    os.remove(tmp)
    ok, problems, m = check_loop.check(dst)
    print(("PASS" if ok else "FAIL") + "  " + "  ".join(f"{k}: {v:.2f}" for k, v in m.items()))
    if not ok:
        print("      " + "; ".join(problems))
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
