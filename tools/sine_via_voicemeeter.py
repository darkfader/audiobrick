#!/usr/bin/env python3
"""Play a steady sine wave from this PC through Voicemeeter and VBAN to the Audio Brick, until Ctrl+C.

  python tools/sine_via_voicemeeter.py [host] [--freq 440] [--db -24] [--seconds N]

Switches Voicemeeter's VBAN stream on, then feeds "Voicemeeter Input" from a callback that keeps the phase continuous
(no clicks, no frequency steps, no restarts), and switches the stream off again at the end. The Brick's own speaker
profile still caps the loudness. Needs Voicemeeter running (see docs/windows-setup.md).
"""
import argparse
import os
import socket
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd

FS = 48000
HERE = Path(__file__).parent


def vban(*a):
    return subprocess.run([sys.executable, str(HERE / "voicemeeter_vban.py"), *a], capture_output=True, text=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="audiobrick.local")
    ap.add_argument("--freq", type=float, default=440.0)
    ap.add_argument("--db", type=float, default=-24.0, help="level in dBFS (default -24)")
    ap.add_argument("--seconds", type=float, default=0.0, help="stop after this long (default: until Ctrl+C)")
    a = ap.parse_args()
    if not 100 <= a.freq <= 4000 or not -60 <= a.db <= -12:
        sys.exit("keep --freq within 100-4000 Hz and --db within -60..-12")
    lock = Path(os.environ.get("TEMP", ".")) / "sine_via_voicemeeter.lock"
    try:  # two copies at once beat against each other and sound like a wobbling tone
        old = int(lock.read_text()) if lock.exists() else 0
        if old and old != os.getpid() and subprocess.run(["tasklist", "/FI", f"PID eq {old}", "/NH"], capture_output=True,
                                                         text=True).stdout.count(str(old)):
            sys.exit(f"another copy is already running (PID {old}); stop it first")
        lock.write_text(str(os.getpid()))
    except OSError:
        pass
    ip = socket.gethostbyname(a.host)  # resolve once; Windows is slow with .local names

    api = sd.query_hostapis()
    dev = next((i for i, d in enumerate(sd.query_devices()) if d["max_output_channels"] > 0 and
                d["name"].startswith("Voicemeeter Input") and "WASAPI" in api[d["hostapi"]]["name"]), None)
    if dev is None:
        sys.exit("no 'Voicemeeter Input' device; is Voicemeeter installed and running?")

    vban(ip, "--name", "Brick", "--route", "0")
    amp = 10 ** (a.db / 20)
    phase = [0.0]
    step = 2 * np.pi * a.freq / FS
    fade = [0]  # samples faded in so far, 20 ms fade-in at the start

    def cb(out, frames, t, status):
        ph = phase[0] + step * np.arange(frames)
        phase[0] = (phase[0] + step * frames) % (2 * np.pi)
        g = np.minimum(1.0, (fade[0] + np.arange(frames)) / (0.02 * FS))
        fade[0] += frames
        out[:, 0] = out[:, 1] = (amp * g * np.sin(ph)).astype("float32")
        if status:
            print("audio glitch:", status, file=sys.stderr)

    print(f"{a.freq:.0f} Hz at {a.db:.0f} dBFS to {ip}; Ctrl+C to stop")
    try:
        with sd.OutputStream(samplerate=FS, channels=2, device=dev, dtype="float32", callback=cb, latency="high"):
            t0 = time.time()
            while not a.seconds or time.time() - t0 < a.seconds:
                time.sleep(0.2)
    except KeyboardInterrupt:
        pass
    finally:
        vban("--off")
        print("stopped, VBAN stream off")


if __name__ == "__main__":
    main()
