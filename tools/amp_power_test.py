#!/usr/bin/env python3
"""Check the amp's idle power-down for audible clicks, with the microphone in front of a speaker.

  python tools/amp_power_test.py [host]        (default audiobrick.local)

The Brick mutes the amp after about a second of silence, then goes Hi-Z after hiz_after_s, then powers the chip
down after off_after_s (/power). This test sets short times (3 s and 8 s), records the speaker the whole time and
watches /power to know when each transition happened. It then looks at the 2-20 kHz band around every transition
(a plain 440 Hz test tone has nothing up there, a click does) and compares it with the room noise and with a
reference: the same tone starting while the amp is already awake. The original times are restored at the end.
Needs AUDIOBRICK_PASSWORD in the environment.
"""
import json
import os
import socket
import sys
import threading
import time
import urllib.request

import numpy as np
import sounddevice as sd
from scipy.signal import butter, sosfilt

# Resolve once: Windows can take 2-3 s per lookup of a .local name, which would wreck the timing below.
HOST = socket.gethostbyname(sys.argv[1] if len(sys.argv) > 1 else "audiobrick.local")
PW = os.environ.get("AUDIOBRICK_PASSWORD", "")
FS = 48000


def call(path, data=None):
    req = urllib.request.Request(f"http://{HOST}{path}", data=data.encode() if data is not None else None,
                                 headers={"X-Token": PW}, method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.read().decode()


def power():
    return json.loads(call("/power"))


def find_mic():
    api = sd.query_hostapis()
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and "Sennheiser Profile" in d["name"] and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    sys.exit("microphone not found")


def main():
    if not PW:
        sys.exit("set AUDIOBRICK_PASSWORD")
    orig = power()
    call("/power", "hiz_after_s=3\noff_after_s=8")
    chunks = []
    t0 = time.time()
    stream = sd.InputStream(samplerate=FS, channels=1, device=find_mic(), dtype="float32",
                            callback=lambda d, f, t, s: chunks.append(d[:, 0].copy()))
    stream.start()

    events = []  # (seconds since start, label)
    stop = threading.Event()

    def watch():
        last = power()["state"]
        while not stop.is_set():
            st = power()["state"]
            if st != last:
                events.append((time.time() - t0, f"{last} -> {st}"))
                last = st
            time.sleep(0.05)

    th = threading.Thread(target=watch)
    th.start()

    def tone(label, secs=1.2):
        events.append((time.time() - t0, label))
        call("/tone?on=1&freq=440&db=-24&vol=-44&ttl=5", "")
        time.sleep(secs)
        call("/tone?on=0", "")

    time.sleep(2.0)
    tone("tone start, amp awake (reference)")
    time.sleep(16.0)          # mute, Hi-Z at ~3 s, power-off at ~8 s, then idle
    tone("tone start from POWER-OFF")
    time.sleep(7.0)           # mute, Hi-Z at ~3 s
    tone("tone start from Hi-Z")
    time.sleep(2.5)
    stop.set(); th.join()
    stream.stop(); stream.close()
    call("/power", f"hiz_after_s={orig['hiz_after_s']}\noff_after_s={orig['off_after_s']}")

    x = np.concatenate(chunks)
    hp = sosfilt(butter(4, [2000, 20000], btype="band", fs=FS, output="sos"), x)
    floor = float(np.sqrt(np.mean(hp[: int(1.5 * FS)] ** 2))) + 1e-9
    print(f"room noise in the 2-20 kHz band: {20 * np.log10(floor):.1f} dBFS rms")
    print(f"{'time':>6}  {'event':<38} {'peak above noise':>17}")
    ref = None
    for t, label in sorted(events):
        a, b = int((t - 0.1) * FS), int((t + 0.6) * FS) if "tone" in label else int((t + 0.4) * FS)
        seg = hp[max(a, 0):b]
        peak = float(np.max(np.abs(seg))) if len(seg) else 0.0
        db = 20 * np.log10(peak / floor)
        if "reference" in label:
            ref = db
        print(f"{t:6.1f}  {label:<38} {db:14.1f} dB")
    if ref is not None:
        worst = 0.0
        for t, label in events:
            if "reference" in label:
                continue
            a, b = int((t - 0.1) * FS), int((t + 0.6) * FS) if "tone" in label else int((t + 0.4) * FS)
            worst = max(worst, 20 * np.log10(float(np.max(np.abs(hp[a:b]))) / floor))
        verdict = "no audible click beyond the normal tone start" if worst <= ref + 6 else "CLICK suspected, listen to it"
        print(f"worst transition {worst:.1f} dB vs reference {ref:.1f} dB -> {verdict}")


if __name__ == "__main__":
    main()
