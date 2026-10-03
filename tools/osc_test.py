#!/usr/bin/env python3
"""Exercise the board's OSC synthesizer and check by microphone that the right notes come out.

  python tools/osc_test.py HOST

Needs the synth switched on (web page, "OSC synthesizer", or POST /synth enabled=1) and the PC microphone in
front of a speaker. Builds real OSC 1.0 packets (no extra libraries): messages with int, float and string
arguments, and one bundle. The amp volume cap and the speaker profile limit apply as always.
"""
import socket
import struct
import sys
import time

import numpy as np
import sounddevice as sd


def pad(b: bytes) -> bytes:
    return b + b"\0" * (4 - len(b) % 4)


def message(addr: str, *args) -> bytes:
    tags = ","
    data = b""
    for a in args:
        if isinstance(a, bool):
            tags += "T" if a else "F"
        elif isinstance(a, int):
            tags += "i"
            data += struct.pack(">i", a)
        elif isinstance(a, float):
            tags += "f"
            data += struct.pack(">f", a)
        else:
            tags += "s"
            data += pad(str(a).encode())
    return pad(addr.encode()) + pad(tags.encode()) + data


def bundle(*msgs: bytes) -> bytes:
    out = pad(b"#bundle") + struct.pack(">Q", 1)
    for m in msgs:
        out += struct.pack(">i", len(m)) + m
    return out


def peaks(x, fs, lo=100, hi=4000, n=3):
    seg = x * np.hanning(len(x))
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    f = np.fft.rfftfreq(1 << 17, 1 / fs)
    sel = (f >= lo) & (f <= hi)
    s, ff = spec[sel], f[sel]
    out = []
    s = s.copy()
    for _ in range(n):
        i = int(np.argmax(s))
        out.append((float(ff[i]), float(s[i])))
        s[max(0, i - 400):i + 400] = 0   # mask the neighbourhood (about 0.5 Hz per bin * 400)
    return out


def main():
    host = sys.argv[1]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    send = lambda b: sock.sendto(b, (host, 9000))
    fs = 48000
    dev = [i for i, d in enumerate(sd.query_devices()) if d["max_input_channels"] > 0 and "Sennheiser Profile" in d["name"]][0]
    total = 17.0
    buf = sd.rec(int(total * fs), samplerate=fs, channels=1, device=dev, dtype="float32")
    t0 = time.perf_counter()

    def at(t, fn):
        d = t0 + t - time.perf_counter()
        if d > 0:
            time.sleep(d)
        fn()

    at(1.0, lambda: send(message("/synth/wave", "sine")))
    at(1.2, lambda: send(message("/synth/note", 69, 100)))                 # A4 = 440 Hz
    at(3.2, lambda: send(message("/synth/note", 76, 100.0)))               # + E5 = 659.3 Hz (float velocity), chord
    at(5.2, lambda: send(message("/synth/alloff")))
    at(6.4, lambda: send(message("/synth/wave", 3)))                       # square by number
    at(6.6, lambda: send(message("/synth/note", 60, 100)))                 # C4 = 261.6 Hz, square has odd harmonics
    at(8.6, lambda: send(message("/synth/noteoff", 60)))
    at(9.8, lambda: send(message("/synth/wave", "sine")))
    at(10.0, lambda: send(message("/synth/note", 24, 100)))                # C1 = 32.7 Hz: below the speaker's lowest frequency
    at(11.5, lambda: send(message("/synth/alloff")))
    at(12.5, lambda: send(bundle(message("/synth/wave", 0), message("/synth/note", 72, 100))))  # C5 = 523.3 Hz via a bundle
    at(14.5, lambda: send(message("/synth/alloff")))
    sd.wait()
    x = buf[:, 0]

    def window(a, b):
        return x[int(a * fs):int(b * fs)]

    def db(a):
        return 20 * np.log10(float(np.sqrt(np.mean(a ** 2))) + 1e-12)

    noise = db(window(0.1, 0.9))
    print(f"room noise {noise:.1f} dBFS")
    ok = True

    def check(label, ok_now, detail):
        nonlocal ok
        ok = ok and ok_now
        print(f"{'OK  ' if ok_now else 'FAIL'} {label}: {detail}")

    p = peaks(window(2.0, 3.0), fs, n=1)[0]
    check("note 69 (A4)", abs(p[0] - 440) < 3, f"loudest {p[0]:.1f} Hz, level {db(window(2.0, 3.0)):.1f} dBFS")
    pk = peaks(window(4.2, 5.0), fs, n=2)
    fr = sorted(round(f) for f, _ in pk)
    check("chord A4 + E5", any(abs(f - 440) < 4 for f in fr) and any(abs(f - 659) < 4 for f in fr), f"two strongest components {fr} Hz")
    pk = peaks(window(7.2, 8.4), fs, lo=150, hi=3000, n=4)
    fr = sorted(round(f) for f, _ in pk)
    check("square wave C4", any(abs(f - 262) < 4 for f in fr) and any(abs(f - 785) < 6 for f in fr), f"strongest components {fr} Hz (expect 262 and the 3rd harmonic near 785)")
    low = db(window(10.3, 11.3))
    check("note below the lowest frequency is ignored", low < noise + 8, f"level {low:.1f} dBFS vs noise {noise:.1f}")
    p = peaks(window(13.2, 14.2), fs, n=1)[0]
    check("note inside an OSC bundle (C5)", abs(p[0] - 523) < 4, f"loudest {p[0]:.1f} Hz")
    tail = db(window(15.6, 16.8))
    check("silence after all notes off", tail < noise + 8, f"level {tail:.1f} dBFS")
    print("RESULT:", "ALL OK" if ok else "SOME CHECKS FAILED")


if __name__ == "__main__":
    main()
