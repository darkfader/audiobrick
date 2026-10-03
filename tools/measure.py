#!/usr/bin/env python3
"""Measure a speaker's frequency response through the Audio Brick with a USB microphone.

  python tools/measure.py HOST --mic "Sennheiser Profile"            # silent check: mic level and noise floor
  python tools/measure.py HOST --mic "Sennheiser Profile" --play     # play one quiet sweep and analyse it

Put the microphone 0.5-1 m in front of ONE speaker, on axis, away from walls and desks.
The sweep goes to one channel only (--channel left|right). It is quiet by design: the script prints the
estimated loudness first and keeps the amp volume under the board's own speaker-profile cap.
The password comes from AUDIOBRICK_PASSWORD or is asked for.

Results go to ./measurements/: a WAV of the recording, a CSV and a PNG of the smoothed response.
The microphone is NOT calibrated, so the result is the speaker plus the microphone plus the room.
Use it for relative corrections (peaks and dips), not for absolute accuracy.
"""
import argparse
import getpass
import json
import os
import socket
import time
import urllib.request
from datetime import datetime

import numpy as np
import sounddevice as sd
from scipy import signal
from scipy.io import wavfile

FS = 48000


def find_mic(name_part):
    matches = [(i, d) for i, d in enumerate(sd.query_devices())
               if d["max_input_channels"] > 0 and name_part.lower() in d["name"].lower()]
    if not matches:
        raise SystemExit(f"no input device containing '{name_part}'. Inputs: " +
                         ", ".join(d["name"] for d in sd.query_devices() if d["max_input_channels"] > 0))
    hostapis = sd.query_hostapis()
    # prefer WASAPI, then the first match
    matches.sort(key=lambda m: 0 if "WASAPI" in hostapis[m[1]["hostapi"]]["name"] else 1)
    return matches[0]


def api(host, password, path, method="GET"):
    req = urllib.request.Request(f"http://{host}{path}", method=method, headers={"X-Token": password},
                                 data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read().decode() or "{}")


def make_sweep(f1, f2, seconds, level_db):
    t = np.arange(int(seconds * FS)) / FS
    k = np.log(f2 / f1)
    x = np.sin(2 * np.pi * f1 * seconds / k * (np.exp(t / seconds * k) - 1))
    fade = int(0.02 * FS)
    x[:fade] *= np.linspace(0, 1, fade)
    x[-fade:] *= np.linspace(1, 0, fade)
    return x * 10 ** (level_db / 20)


def send_stream(host, password, stereo_int16):
    s = socket.create_connection((host, 4010), timeout=10)
    s.sendall(f"STREAM {password} 48000 2\n".encode())
    reply = b""
    while not reply.endswith(b"\n"):
        c = s.recv(1)
        if not c:
            break
        reply += c
    if reply.strip() != b"OK":
        raise SystemExit(f"board refused the stream: {reply!r}")
    s.settimeout(None)
    s.sendall(stereo_int16.tobytes())
    s.close()


def record(dev_index, dev, seconds):
    fs = int(dev["default_samplerate"]) if int(dev["default_samplerate"]) != FS else FS
    ch = min(2, dev["max_input_channels"])
    buf = sd.rec(int(seconds * fs), samplerate=fs, channels=ch, device=dev_index, dtype="float32")
    return buf, fs


def smooth_octave(freqs, mag_db, frac=6):
    out = np.empty_like(mag_db)
    for i, f in enumerate(freqs):
        if f <= 0:
            out[i] = mag_db[i]
            continue
        lo, hi = f * 2 ** (-1 / (2 * frac)), f * 2 ** (1 / (2 * frac))
        sel = (freqs >= lo) & (freqs <= hi)
        out[i] = np.mean(mag_db[sel]) if sel.any() else mag_db[i]
    return out


def mic_correction(path, freqs):
    """dB to SUBTRACT from the measured curve so the microphone's own response is removed."""
    if not path or not os.path.exists(path):
        return np.zeros_like(freqs)
    data = np.loadtxt(path, delimiter=",", skiprows=1)
    return np.interp(np.log10(freqs), np.log10(data[:, 0]), data[:, 1])


def analyse(rec, x, gate_ms):
    n = 1 << int(np.ceil(np.log2(len(rec) + len(x))))
    X = np.fft.rfft(x, n)
    Y = np.fft.rfft(rec, n)
    eps = 1e-4 * np.max(np.abs(X) ** 2)
    h = np.fft.irfft(Y * np.conj(X) / (np.abs(X) ** 2 + eps), n)
    peak = int(np.argmax(np.abs(h[: FS * 2])))
    start = max(0, peak - int(0.002 * FS))
    results = {}
    for name, ms in (("short", gate_ms), ("long", 60.0)):
        length = int(ms / 1000 * FS)
        seg = h[start:start + length].copy()
        taper = signal.windows.hann(2 * (length // 4))[length // 4:]  # fade out the last quarter
        seg[-len(taper):] *= taper
        nfft = 1 << 16
        H = np.fft.rfft(seg, nfft)
        f = np.fft.rfftfreq(nfft, 1 / FS)
        results[name] = (f, 20 * np.log10(np.abs(H) + 1e-12))
    return results, peak / FS


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("--mic", default="Sennheiser Profile")
    ap.add_argument("--play", action="store_true", help="actually play the sweep (default: silent check only)")
    ap.add_argument("--channel", choices=["left", "right"], default="left")
    ap.add_argument("--level", type=float, default=-12.0, help="sweep level in dBFS (default -12)")
    ap.add_argument("--vol", type=int, default=-33, help="amp volume in dB while measuring (default -33)")
    ap.add_argument("--f1", type=float, default=80.0)
    ap.add_argument("--f2", type=float, default=20000.0)
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--gate-ms", type=float, default=8.0, help="window after the direct sound (hides room echoes)")
    ap.add_argument("--label", default="speaker")
    ap.add_argument("--mic-curve", default=os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                                         "mic_sennheiser_profile_0deg.csv"),
                    help="CSV of the microphone's own response (freq_hz,db); removed from the result. 'none' to skip")
    ap.add_argument("--analyse", metavar="WAV", help="re-analyse a saved recording; nothing is played")
    args = ap.parse_args()
    if args.mic_curve == "none":
        args.mic_curve = None

    if args.analyse:
        fs_in, data = wavfile.read(args.analyse)
        sweep = make_sweep(args.f1, args.f2, args.seconds, args.level)
        report(data.astype(float) / 32768, sweep, args, os.path.splitext(args.analyse)[0] + "-corrected")
        return

    idx, dev = find_mic(args.mic)
    print(f"microphone: [{idx}] {dev['name']} ({dev['default_samplerate']:.0f} Hz default)")
    password = os.environ.get("AUDIOBRICK_PASSWORD") or getpass.getpass("Audio Brick password: ")

    if not args.play:
        buf, fs = record(idx, dev, 2.0)
        sd.wait()
        rms = np.sqrt(np.mean(buf ** 2))
        print(f"silent check: input level {20 * np.log10(rms + 1e-12):.1f} dBFS rms, peak {20 * np.log10(np.max(np.abs(buf)) + 1e-12):.1f} dBFS")
        print("(room noise floor; the sweep should come out at least 20 dB above this)")
        return

    prof = api(args.host, password, "/profile")
    st = api(args.host, password, "/status")
    sens, dist, cap = prof["sens"], prof["dist"], prof["max_vol_db"]
    vol = min(args.vol, cap)
    vrms = 29.5 * 10 ** ((args.level + vol) / 20) / np.sqrt(2)
    spl = sens + 20 * np.log10(vrms / 2.83) - 20 * np.log10(dist)
    print(f"estimated peak loudness at {dist:g} m: about {spl:.0f} dB SPL (limit {prof['maxspl']:.0f}); amp volume {vol} dB")
    if spl > prof["maxspl"]:
        raise SystemExit("refusing: this would exceed the speaker profile's limit")
    if st["media"]["src"] != "none" or st["tone"]["on"]:
        raise SystemExit("the board is playing something; stop it first")

    sweep = make_sweep(args.f1, args.f2, args.seconds, args.level)
    left = args.channel == "left"
    pcm = np.zeros((len(sweep) + int(0.3 * FS), 2), dtype=np.int16)  # 0.3 s of silence after the sweep
    pcm[: len(sweep), 0 if left else 1] = (sweep * 32767).astype(np.int16)

    old_vol = st["vol_db"]
    api(args.host, password, f"/volume?db={vol}", "POST")
    try:
        total = 0.8 + args.seconds + 2.5
        buf, fs = record(idx, dev, total)
        time.sleep(0.8)
        send_stream(args.host, password, pcm)
        sd.wait()
    finally:
        api(args.host, password, f"/volume?db={old_vol}", "POST")  # back to the previous volume

    rec = buf.mean(axis=1) if buf.shape[1] > 1 else buf[:, 0]
    if fs != FS:
        rec = signal.resample_poly(rec, FS, fs)
    peak = 20 * np.log10(np.max(np.abs(rec)) + 1e-12)
    print(f"recording peak {peak:.1f} dBFS" + ("  (CLIPPING: lower the mic gain!)" if peak > -1 else ""))

    os.makedirs("measurements", exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    base = f"measurements/{args.label}-{args.channel}-{stamp}"
    wavfile.write(base + ".wav", FS, (rec * 32767).clip(-32768, 32767).astype(np.int16))
    report(rec, sweep, args, base)


def report(rec, sweep, args, base):
    results, delay = analyse(rec, sweep, args.gate_ms)
    print(f"direct sound arrives {delay * 1000:.1f} ms after the start (latency + distance)")

    f, short = results["short"]
    _, long_ = results["long"]
    grid = np.geomspace(60, 20000, 400)
    mic = mic_correction(args.mic_curve, grid)
    s_short = np.interp(grid, f, smooth_octave(f, short, 6)) - mic
    s_long = np.interp(grid, f, smooth_octave(f, long_, 6)) - mic
    ref = np.mean(s_short[(grid > 500) & (grid < 4000)])
    print("microphone response removed" if args.mic_curve else "microphone response NOT removed")
    np.savetxt(base + ".csv", np.column_stack([grid, s_short - ref, s_long - ref]), delimiter=",",
               header="freq_hz,gated_db,long_db (1/6 octave, relative to the 0.5-4 kHz mean)", comments="")

    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    plt.figure(figsize=(10, 5))
    plt.semilogx(grid, s_short - ref, label=f"gated {args.gate_ms:g} ms")
    plt.semilogx(grid, s_long - ref, label="60 ms window", alpha=0.6)
    plt.grid(True, which="both", alpha=0.3)
    plt.xlim(60, 20000)
    plt.ylim(-30, 15)
    plt.xlabel("Hz")
    plt.ylabel("dB relative")
    plt.title(f"{args.label} {args.channel} (1/6 octave, uncalibrated mic)")
    plt.legend()
    plt.savefig(base + ".png", dpi=130, bbox_inches="tight")
    print("saved", base + ".png", ".csv", ".wav")
    for f0 in (100, 200, 400, 1000, 2000, 4000, 8000, 12000, 16000):
        print(f"  {f0:>6} Hz: {np.interp(f0, grid, s_short - ref):+5.1f} dB")


if __name__ == "__main__":
    main()
