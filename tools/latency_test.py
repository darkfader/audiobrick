#!/usr/bin/env python3
"""Measure startup times with the PC microphone.

  python tools/latency_test.py HOST            # command-to-sound latency, amp idle versus warm
  python tools/latency_test.py HOST --boot     # also reset the board over the serial port and time its start-up

The microphone has its own input latency (typically 10-50 ms), which is included in every number, so read the
numbers as "about", and compare them with each other. Needs: the synth switched on, clips on the board, the
password in AUDIOBRICK_PASSWORD, and for --boot the serial port COM5 free.
"""
import os
import subprocess
import sys
import time
import urllib.request

import numpy as np
import sounddevice as sd

sys.path.insert(0, os.path.dirname(__file__))
from osc_test import message  # noqa: E402

FS = 48000
HOST = sys.argv[1]
PW = os.environ["AUDIOBRICK_PASSWORD"]


def http(path, method="POST", timeout=5):
    req = urllib.request.Request(f"http://{HOST}{path}", method=method, headers={"X-Token": PW}, data=b"" if method == "POST" else None)
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode()


def mic():
    return [i for i, d in enumerate(sd.query_devices()) if d["max_input_channels"] > 0 and "Sennheiser Profile" in d["name"]][0]


def onset(x, t_cmd, noise_db, margin=12.0):
    """Time (s, relative to the command) when the signal first rises margin dB above the noise."""
    w = int(0.005 * FS)
    thr = 10 ** ((noise_db + margin) / 20)
    start = int(t_cmd * FS)
    for i in range(start, len(x) - w, w // 2):
        if np.sqrt(np.mean(x[i:i + w] ** 2)) > thr:
            return i / FS - t_cmd
    return None


def measure(label, trigger, settle=4.0, record=2.5):
    time.sleep(settle)  # let the amp go idle (it mutes after about 1 s of silence plus the fade)
    buf = sd.rec(int((record + 0.6) * FS), samplerate=FS, channels=1, device=mic(), dtype="float32")
    t0 = time.perf_counter()
    time.sleep(0.5)
    t_cmd = time.perf_counter() - t0
    trigger()
    sd.wait()
    x = buf[:, 0]
    noise = 20 * np.log10(float(np.sqrt(np.mean(x[: int(0.4 * FS)] ** 2))) + 1e-12)
    d = onset(x, t_cmd, noise)
    peak = 20 * np.log10(float(np.max(np.abs(x[int(t_cmd * FS):]))) + 1e-12)
    print(f"{label:<44} {('%4.0f ms' % (d * 1000)) if d is not None else 'not heard'}   (noise {noise:.0f} dBFS, loudest {peak:.0f} dBFS)")
    return d


def main():
    import socket
    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    note = lambda n: udp.sendto(message("/synth/note", n, 127), (HOST, 9000))
    http("/synth/note?note=60&vel=1&dur=50")  # make sure the path works
    print("command-to-sound latency (includes the microphone's own delay):")
    alloff = lambda: udp.sendto(message("/synth/alloff"), (HOST, 9000))
    measure("OSC note, amp idle (muted)", lambda: note(69))
    alloff()
    def warm():
        note(60)
        time.sleep(0.6)
        note(69)
    # the second note is the one we time, so trigger the first earlier and measure from the second
    time.sleep(4.0)
    buf = sd.rec(int(3.5 * FS), samplerate=FS, channels=1, device=mic(), dtype="float32")
    t0 = time.perf_counter()
    time.sleep(0.4)
    note(48 + 12 + 0)          # first note wakes the amp
    time.sleep(0.9)
    note(81)                   # second note while the amp is awake: A5 (880 Hz) so we can tell it from the first
    t_cmd = time.perf_counter() - t0
    sd.wait()
    x = buf[:, 0]
    # onset of the 880 Hz component only
    from scipy import signal
    sos = signal.butter(4, [800, 960], "bandpass", fs=FS, output="sos")
    y = signal.sosfilt(sos, x)
    noise = 20 * np.log10(float(np.sqrt(np.mean(y[: int(0.3 * FS)] ** 2))) + 1e-12)
    d = onset(y, t_cmd, noise, margin=15.0)
    print(f"{'OSC note, amp already awake':<44} {('%4.0f ms' % (d * 1000)) if d is not None else 'not heard'}")
    alloff()
    measure("play a clip (keyboard_typing_1.mp3), amp idle", lambda: http("/clips/play?name=keyboard_typing_1.mp3"))
    http("/media/stop")
    measure("start the test tone, amp idle (ramps up over 100 ms)", lambda: http("/tone?on=1&freq=1000&db=-12&vol=-33"))
    http("/tone?on=0")

    if "--boot" in sys.argv:
        import serial  # pyserial
        print("\nboot time (reset over the serial port):")
        time.sleep(3)
        buf = sd.rec(int(40 * FS), samplerate=FS, channels=1, device=mic(), dtype="float32")
        t0 = time.perf_counter()
        time.sleep(0.5)
        s = serial.Serial("COM5", 115200, dsrdtr=False, rtscts=False)
        s.dtr = False
        s.rts = True
        time.sleep(0.1)
        s.rts = False
        t_reset = time.perf_counter() - t0
        s.close()
        t_http = None
        t_sound = None
        deadline = time.perf_counter() + 30
        while time.perf_counter() < deadline and (t_http is None or t_sound is None):
            if t_http is None:
                try:
                    http("/status", "GET", timeout=0.3)
                    t_http = time.perf_counter() - t0
                except Exception:
                    pass
            if t_sound is None:
                note(81)
            time.sleep(0.1)
            # check the microphone for the note
            if t_sound is None:
                recorded = int((time.perf_counter() - t0) * FS)
                seg = buf[max(0, recorded - FS // 4):recorded, 0]
                if len(seg) > FS // 10 and np.sqrt(np.mean(seg ** 2)) > 10 ** (-60 / 20):
                    t_sound = time.perf_counter() - t0 - 0.1
        sd.stop()
        print(f"web page answers {t_http - t_reset:.1f} s after the reset" if t_http else "web page did not answer")
        print(f"first note heard {t_sound - t_reset:.1f} s after the reset" if t_sound else "no note heard")


if __name__ == "__main__":
    main()
