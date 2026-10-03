#!/usr/bin/env python3
"""How long does the amp take to wake, and is the start of a sound lost?  (microphone in front of a speaker)

  python tools/wake_test.py [host] [--repeat N]

For each amp state (awake, Hi-Z, powered off) it streams 1.0 s of an abruptly starting 600 Hz tone to the stream port
and records the speaker. It reports when the tone becomes audible (relative to sending) and how long it lasts. The
difference between the states is the wake-up cost; a shorter tone than the awake one would mean the start was dropped.
The mic's own delay (about 40-50 ms) is the same in every row. Needs AUDIOBRICK_PASSWORD. Temporarily sets short power
times (3 s / 8 s) and restores them.
"""
import argparse
import json
import os
import socket
import sys
import time
import urllib.request

import numpy as np
import sounddevice as sd
from scipy.signal import butter, sosfilt

FS = 48000
FREQ = 600.0
PW = os.environ.get("AUDIOBRICK_PASSWORD", "")


def http(host, path, data=None):
    req = urllib.request.Request(f"http://{host}{path}", data=data.encode() if data is not None else None,
                                 headers={"X-Token": PW}, method="POST" if data is not None else "GET")
    with urllib.request.urlopen(req, timeout=5) as r:
        return r.read().decode()


def state(host):
    return json.loads(http(host, "/power"))["state"]


def wait_state(host, want, timeout):
    t = time.time()
    while time.time() - t < timeout:
        if state(host) == want:
            return True
        time.sleep(0.1)
    return False


def send_tone(host, secs=1.0, level_db=-12.0):
    n = int(secs * FS)
    x = (10 ** (level_db / 20) * np.sin(2 * np.pi * FREQ * np.arange(n) / FS) * 32767).astype("<i2")
    pcm = np.column_stack([x, x]).tobytes()
    t_send = time.time()
    s = socket.create_connection((host, 4010), timeout=5)
    s.sendall(f"STREAM {PW} {FS} 2\n".encode())
    s.sendall(pcm)
    time.sleep(secs + 0.6)
    s.close()
    return t_send


def send_tone_direct(host, secs=1.0, level_db=-12.0):
    """The web page's test tone: no pre-buffer, so a slow wake-up shows up fully."""
    t_send = time.time()
    http(host, f"/tone?on=1&freq={FREQ:.0f}&db=-24&ttl=5", "")
    time.sleep(secs)
    http(host, "/tone?on=0", "")
    time.sleep(0.6)
    return t_send


def mic_index():
    api = sd.query_hostapis()
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and "Sennheiser Profile" in d["name"] and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    sys.exit("microphone not found")


def measure(host, label, prep, mic, send):
    prep()
    chunks = []
    t0 = [None]

    def cb(d, f, t, s):
        if t0[0] is None:
            t0[0] = time.time() - len(d) / FS
        chunks.append(d[:, 0].copy())

    ins = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32", callback=cb)
    ins.start()
    time.sleep(0.5)
    st = state(host)
    t_send = send(host)
    time.sleep(0.3)
    ins.stop(); ins.close()
    x = np.concatenate(chunks)
    band = sosfilt(butter(4, [500, 700], btype="band", fs=FS, output="sos"), x)
    env = np.abs(band)
    k = int(0.005 * FS)
    env = np.convolve(env, np.ones(k) / k, mode="same")
    peak = float(env.max())
    on = np.where(env > 0.5 * peak)[0]
    if peak < 20 * float(np.median(env[: int(0.4 * FS)]) + 1e-9) or len(on) == 0:
        return label, st, None, None
    onset = on[0] / FS - (t_send - t0[0])
    dur = (on[-1] - on[0]) / FS
    return label, st, onset * 1000, dur * 1000


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="audiobrick.local")
    ap.add_argument("--repeat", type=int, default=2)
    ap.add_argument("--path", choices=["stream", "tone"], default="stream", help="how the sound is sent")
    a = ap.parse_args()
    if not PW:
        sys.exit("set AUDIOBRICK_PASSWORD")
    host = socket.gethostbyname(a.host)  # resolve once (Windows is slow with .local names)
    orig = json.loads(http(host, "/power"))
    http(host, "/power", "hiz_after_s=3\noff_after_s=8")
    mic = mic_index()

    def awake():
        http(host, "/power", "hiz_after_s=0\noff_after_s=0")  # keep it awake
        send_tone(host, 0.3, -30)                             # wake it and let it settle
        time.sleep(1.5)

    def hiz():
        http(host, "/power", "hiz_after_s=3\noff_after_s=0")
        if not wait_state(host, "hiz", 30):
            sys.exit("amp never went Hi-Z")

    def off():
        http(host, "/power", "hiz_after_s=3\noff_after_s=8")
        if not wait_state(host, "off", 40):
            sys.exit("amp never powered down")

    rows = []
    try:
        for _ in range(a.repeat):
            for label, prep in (("awake", awake), ("Hi-Z", hiz), ("powered off", off)):
                rows.append(measure(host, label, prep, mic, send_tone if a.path == "stream" else send_tone_direct))
    finally:
        http(host, "/power", f"hiz_after_s={orig['hiz_after_s']}\noff_after_s={orig['off_after_s']}")
    print(f"{'amp state':<12} {'state at send':<14} {'audible after':>14} {'tone lasted':>12}")
    base = [r[2] for r in rows if r[0] == "awake" and r[2] is not None]
    for label, st, onset, dur in rows:
        if onset is None:
            print(f"{label:<12} {st:<14} {'not heard':>14}")
        else:
            print(f"{label:<12} {st:<14} {onset:11.0f} ms {dur:9.0f} ms")
    for lab in ("Hi-Z", "powered off"):
        v = [r[2] for r in rows if r[0] == lab and r[2] is not None]
        if v and base:
            print(f"wake cost {lab}: {np.mean(v) - np.mean(base):+.0f} ms vs awake")


if __name__ == "__main__":
    main()
