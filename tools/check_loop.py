#!/usr/bin/env python3
"""Check whether audio clips loop cleanly. Only clips that pass should be named *_loop.mp3.

  python tools/check_loop.py clips/*_loop.mp3
  python tools/check_loop.py clips/rain_window_loop.mp3 --verbose

The check decodes the clip the way the board plays it (gapless: encoder delay and padding removed), repeats it twice
and looks at the join:
  level step    loudness of the last 0.5 s versus the first 0.5 s, in dB          (must be within 2.0 dB)
  tone change   how different the spectrum is across the join (1/3-octave bands)  (mean difference within 3.0 dB)
  click         sample step at the join compared with the largest steps elsewhere in the clip   (within 1.5x)
  dip/bump      loudness of the 40 ms around the join versus the surrounding second, measured in
                multiples of how much that measure varies by itself elsewhere in the clip       (within 2.5)
Needs ffmpeg on the PATH and numpy.
"""
import subprocess
import sys

import numpy as np

FS = 48000


def decode(path):
    """Mono 48 kHz float samples, with the MP3 encoder delay and padding trimmed (ffmpeg does this from the LAME tag)."""
    out = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-f", "f32le", "-ac", "1", "-ar", str(FS), "pipe:1"],
                         capture_output=True, check=True).stdout
    return np.frombuffer(out, dtype="<f4")


def rms_db(x):
    return 20 * np.log10(float(np.sqrt(np.mean(x ** 2))) + 1e-9)


def band_levels(x):
    """Spectrum in 1/3-octave bands from 100 Hz to 12 kHz, in dB."""
    spec = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
    f = np.fft.rfftfreq(len(x), 1 / FS)
    out = []
    fc = 100.0
    while fc < 12000:
        lo, hi = fc / 2 ** (1 / 6), fc * 2 ** (1 / 6)
        sel = (f >= lo) & (f < hi)
        out.append(10 * np.log10(float(spec[sel].sum()) + 1e-12))
        fc *= 2 ** (1 / 3)
    return np.array(out)


def check(path, verbose=False):
    x = decode(path)
    if len(x) < 4 * FS:
        return False, ["clip shorter than 4 s"], {}
    n = int(0.5 * FS)
    a, b = x[:n], x[-n:]
    step = abs(rms_db(b) - rms_db(a))
    ba, bb = band_levels(a), band_levels(b)
    ba -= ba.mean()
    bb -= bb.mean()
    tone = float(np.mean(np.abs(ba - bb)))
    # click: the sample step at the join compared with the largest steps found anywhere else in the clip
    d = np.abs(np.diff(x))
    seam_step = abs(float(x[0]) - float(x[-1]))
    jump = seam_step / (float(np.percentile(d, 99.9)) + 1e-9)
    # dip/bump: loudness of the 40 ms around the join versus the second around it, judged against how much the
    # same measure varies at other places in the same clip (noise-like sounds fluctuate on their own)
    loop2 = np.concatenate([x, x])
    j = len(x)
    w = int(0.02 * FS)
    def local_dev(c):
        seg = loop2[c - w: c + w]
        ctx = np.concatenate([loop2[c - FS: c - 5 * w], loop2[c + 5 * w: c + FS]])
        return rms_db(seg) - rms_db(ctx)
    seam_dev = local_dev(j)
    rng = np.random.default_rng(1)
    others = [local_dev(int(c)) for c in rng.integers(FS + w, len(x) - FS - w, 300)]
    sigma = float(np.std(others)) + 0.3
    dip = abs(seam_dev) / sigma   # how many "natural fluctuations" the join stands out by
    metrics = {"level step (dB)": step, "tone change (dB)": tone, "click (x largest elsewhere)": jump, "dip/bump at join (sigmas)": dip}
    limits = {"level step (dB)": 2.0, "tone change (dB)": 3.0, "click (x largest elsewhere)": 1.5, "dip/bump at join (sigmas)": 2.5}
    problems = [f"{k} {v:.1f} > {limits[k]}" for k, v in metrics.items() if v > limits[k]]
    return not problems, problems, metrics


def main():
    files = [a for a in sys.argv[1:] if not a.startswith("--")]
    verbose = "--verbose" in sys.argv
    if not files:
        sys.exit(__doc__)
    all_ok = True
    for f in files:
        ok, problems, m = check(f, verbose)
        all_ok &= ok
        name = f.replace("\\", "/").split("/")[-1]
        print(f"{'PASS' if ok else 'FAIL'}  {name:<28}" + ("" if ok else "  " + "; ".join(problems)))
        if verbose or not ok:
            print("      " + "  ".join(f"{k}: {v:.2f}" for k, v in m.items()))
    sys.exit(0 if all_ok else 1)


if __name__ == "__main__":
    main()
