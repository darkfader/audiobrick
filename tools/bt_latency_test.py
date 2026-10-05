#!/usr/bin/env python3
"""Latency of "Windows app -> Bluetooth -> Audio Brick -> speaker", measured with the microphone.  (Needs Windows' Bluetooth audio connected to the Brick, see docs/bluetooth.md.)

  python tools/bt_latency_test.py [--out "Audio Brick"] [--repeat 3]

Plays 150 ms bursts of 600 Hz into the Windows playback device whose name contains --out (the Bluetooth audio connection shows up as
something like "Headphones (Audio Brick Stereo)"), records the speaker with the Sennheiser microphone and reports the time from the start of
each burst (as handed to Windows) to the moment the microphone hears it. That includes Windows' audio engine, the Bluetooth encoder and radio,
the Brick's buffer (see /latency) and output stage, and the microphone's own ~45 ms. For a reference run the same script with
--out "Speakers (VB-Audio Virtual Cable" (the wired route, whose parts are known, see tools/cable_latency_test.py) and compare.
"""
import argparse
import sys
import time

import numpy as np
import sounddevice as sd
from scipy.signal import butter, sosfiltfilt

FS = 48000


def find(prefix, kind):
    api = sd.query_hostapis()
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    for i, d in enumerate(sd.query_devices()):
        if d[key] > 0 and prefix.lower() in d["name"].lower() and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    sys.exit(f"no WASAPI {kind}put device containing '{prefix}'")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="Audio Brick")
    ap.add_argument("--repeat", type=int, default=3)
    a = ap.parse_args()
    out_dev = find(a.out, "out")
    mic = find("Microphone (Sennheiser Profile", "in")
    SR = int(sd.query_devices(out_dev)["default_samplerate"])   # the Bluetooth device runs at 44.1 kHz in Windows' shared mode
    n_bursts, gap = 8, 1.5
    total = int((2.0 + n_bursts * gap + 1.5) * SR)
    x = np.zeros(total, dtype="float32")
    starts = [int((2.0 + k * gap) * SR) for k in range(n_bursts)]
    t = np.arange(total) / SR
    x[: int(2.0 * SR)] = 0.05 * np.sin(2 * np.pi * 300 * t[: int(2.0 * SR)])   # quiet lead-in: opens the connection and lets buffers settle
    for s0 in starts:
        n = int(0.15 * SR)
        x[s0:s0 + n] = 0.4 * np.sin(2 * np.pi * 600 * np.arange(n) / SR)

    results = []
    for rep in range(a.repeat):
        pos, t0, rec = [0], [None], []
        ins = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32",
                             callback=lambda d, f, ti, s: rec.append((time.perf_counter(), d[:, 0].copy())))

        def cb(out, frames, ti, s):
            if t0[0] is None:
                t0[0] = time.perf_counter()
            ch = x[pos[0]:pos[0] + frames]
            out[:len(ch), 0] = out[:len(ch), 1] = ch
            out[len(ch):] = 0
            pos[0] += frames

        ins.start()
        with sd.OutputStream(samplerate=SR, channels=2, device=out_dev, dtype="float32", callback=cb):
            time.sleep(total / SR + 1.0)
        ins.stop(); ins.close()
        ts = np.concatenate([np.full(len(d), tt) - (len(d) - np.arange(len(d))) / FS for tt, d in rec])
        v = np.concatenate([d for _, d in rec]).astype("float64")
        env = np.convolve(np.abs(sosfiltfilt(butter(4, [520, 680], btype="band", fs=FS, output="sos"), v)), np.ones(96) / 96, mode="same")
        for k, s0 in enumerate(starts):
            lo = t0[0] + s0 / SR - 0.1
            sel = np.where((ts > lo) & (ts < lo + 1.2))[0]
            if len(sel) == 0:
                continue
            w = env[sel]
            floor = float(np.median(env[(ts > lo - 0.5) & (ts < lo)])) if np.any((ts > lo - 0.5) & (ts < lo)) else 0.0
            if w.max() < 4 * max(floor, 1e-6):
                continue
            onset = ts[sel[np.argmax(w > 0.5 * w.max())]]
            results.append((onset - (t0[0] + s0 / SR)) * 1000)
    if not results:
        sys.exit("nothing heard at the speaker")
    r = np.array(results)
    print(f"{len(r)} bursts heard: median {np.median(r):.0f} ms, min {r.min():.0f}, max {r.max():.0f} (includes the mic's own ~45 ms)")


if __name__ == "__main__":
    main()
