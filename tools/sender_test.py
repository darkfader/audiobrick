#!/usr/bin/env python3
"""Test the background sender (tools/brick_sender.py) end to end, with the microphone in front of a speaker.

  python tools/sender_test.py [host]        (brick_sender.py must be running; AUDIOBRICK_PASSWORD needed for the status calls only if you add them)

Plays 3 s of 600 Hz into VB-Cable's playback device (what a Windows app would do), records the speaker, then waits and checks
on the Brick that the stream was released again after the silence (so that the amp can idle). Reports the pitch and the Brick's
state before / during / after.
"""
import json
import socket
import sys
import time
import urllib.request

import numpy as np
import sounddevice as sd

HOST = socket.gethostbyname(sys.argv[1] if len(sys.argv) > 1 else "audiobrick.local")
FS = 48000
FREQ = 600.0


def status():
    with urllib.request.urlopen(f"http://{HOST}/status", timeout=4) as r:
        s = json.loads(r.read())
    return s["media"]["src"], s["amp"]


def find(prefix, kind):
    api = sd.query_hostapis()
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    for i, d in enumerate(sd.query_devices()):
        if d[key] > 0 and d["name"].startswith(prefix) and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    sys.exit(f"device '{prefix}' not found")


def main():
    cable = find("Speakers (VB-Audio Virtual Cable", "out")
    mic = find("Microphone (Sennheiser Profile", "in")
    print("before:", status())
    chunks = []
    ins = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32", callback=lambda d, f, t, s: chunks.append(d[:, 0].copy()))
    ins.start()
    time.sleep(0.8)
    t_start = time.time()
    n = [0]

    def cb(out, frames, t, s):
        ph = (np.arange(frames) + n[0]) / FS
        n[0] += frames
        out[:, 0] = out[:, 1] = 0.3 * np.sin(2 * np.pi * FREQ * ph)

    with sd.OutputStream(samplerate=FS, channels=2, device=cable, dtype="float32", callback=cb):
        time.sleep(2.0)
        print("during:", status())
        time.sleep(1.0)
    time.sleep(0.5)
    ins.stop(); ins.close()
    x = np.concatenate(chunks)
    env = np.convolve(np.abs(x), np.ones(480) / 480, mode="same")
    noise = float(np.median(env[: int(0.6 * FS)])) + 1e-9
    on = np.where(env > 10 * noise)[0]
    peak = float(np.max(env))
    if len(on) == 0 or peak < 20 * noise:
        print("RESULT: nothing heard at the speaker")
    else:
        i0 = on[0]
        win = x[i0 + int(0.5 * FS): i0 + int(1.5 * FS)] * np.hanning(FS)
        f = np.fft.rfftfreq(1 << 17, 1 / FS)
        sp = np.abs(np.fft.rfft(win, 1 << 17))
        pk = float(f[(f > 100)][np.argmax(sp[f > 100])])
        print(f"heard {pk:.1f} Hz at the speaker (expected {FREQ:.0f})")
    time.sleep(9.0)  # the sender lets go of the Brick 5 s after the last sound
    print("after silence:", status())


if __name__ == "__main__":
    main()
