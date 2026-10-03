#!/usr/bin/env python3
"""What does tools/brick_sender.py do when the Brick stops reading (reboot during a firmware update, crash, cable pulled)?

  python tools/sender_stall_test.py

Starts a FAKE Brick on 127.0.0.1:4010 and a separate copy of the sender pointed at it (stop the installed sender first:
`Stop-ScheduledTask AudioBrickSender`, start it again afterwards). Plays a rising tone (200 Hz + 90 Hz per second) into VB-Cable's
playback device so that the pitch tells how old the audio is, lets the fake Brick read for 3 s, then stops reading for 6 s (the connection
stays open, like a rebooting Brick that never sends a reset), then accepts the reconnect. Reports how long the sender needed to
reconnect and how old the first audio after the reconnect was. Healthy: reconnect within about 3 s, audio age under 1 s.
(Before the fix the sender blocked for minutes and replayed up to 8 s of old audio: the "enormous video/audio lag" of a firmware update.)
"""
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import numpy as np
import sounddevice as sd

FS = 48000
HERE = Path(__file__).parent
events = []          # (time, text)
chunks = {}          # connection number -> list of (time received, bytes)
T0 = time.time()


def log(msg):
    events.append((time.time() - T0, msg))


def fake_brick():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 8192)   # a small window like the Brick's lwIP, so a stall shows quickly
    srv.bind(("127.0.0.1", 4010))
    srv.listen(4)
    n = 0

    def handle(c, n):
        f = c.makefile("rb")
        f.readline()
        c.sendall(b"OK" + bytes([10]))
        chunks[n] = []
        log(f"fake Brick: connection {n} accepted")
        t_conn = time.time()
        c.settimeout(0.2)
        if n == 1:
            while time.time() - t_conn < 3.0:       # read for 3 s ...
                try:
                    d = c.recv(65536)
                except socket.timeout:
                    continue
                if not d:
                    break
                chunks[n].append((time.time(), d))
            log("fake Brick: stops reading (connection 1 stays open, like a Brick that never sends a reset)")
            time.sleep(8.0)
            c.close()
        else:
            end = time.time() + 6.0
            while time.time() < end:
                try:
                    d = c.recv(65536)
                except socket.timeout:
                    continue
                if not d:
                    break
                chunks[n].append((time.time(), d))
            c.close()

    while True:                                      # a new connection is accepted at once, like a Brick that came back up
        c, _ = srv.accept()
        n += 1
        threading.Thread(target=handle, args=(c, n), daemon=True).start()


def main():
    threading.Thread(target=fake_brick, daemon=True).start()
    env = dict(os.environ, AUDIOBRICK_PASSWORD="test")
    sender = subprocess.Popen([sys.executable, str(HERE / "brick_sender.py"), "127.0.0.1"], env=env,
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(3)
    api = sd.query_hostapis()
    dev = next(i for i, d in enumerate(sd.query_devices()) if d["max_output_channels"] > 0 and
               d["name"].startswith("Speakers (VB-Audio Virtual Cable") and "WASAPI" in api[d["hostapi"]]["name"])
    pos = [0]
    t_audio0 = [None]

    def cb(out, frames, t, s):
        if t_audio0[0] is None:
            t_audio0[0] = time.time()
        tt = (np.arange(frames) + pos[0]) / FS
        pos[0] += frames
        ph = 2 * np.pi * (200 * tt + 45 * tt * tt)           # f(t) = 200 + 90 t
        out[:, 0] = out[:, 1] = (0.3 * np.sin(ph)).astype("float32")

    with sd.OutputStream(samplerate=FS, channels=2, device=dev, dtype="float32", callback=cb):
        time.sleep(18)
    sender.terminate()

    first_wall = chunks.get(1, [[None]])[0][0] if chunks.get(1) else None
    print("events:")
    for t, m in events:
        print(f"  {t:6.1f} s  {m}")
    if 2 not in chunks or not chunks[2]:
        print("RESULT: the sender never reconnected with audio")
        return
    stall_start = next(t for t, m in events if "stops reading" in m)
    reconnect = next(t for t, m in events if "connection 2 accepted" in m)
    print(f"reconnect {reconnect - stall_start:.1f} s after the Brick stopped reading")
    t_first, data = chunks[2][0]
    pcm = np.frombuffer(b"".join(d for _, d in chunks[2][:4]), dtype="<i2").reshape(-1, 2)[:, 0].astype(float)
    seg = pcm[:8192] * np.hanning(len(pcm[:8192]))
    f = np.fft.rfftfreq(1 << 16, 1 / FS)
    sp = np.abs(np.fft.rfft(seg, 1 << 16))
    freq = float(f[np.argmax(sp)])
    t_in_audio = (freq - 200.0) / 90.0 + 0.085                 # time within the chirp at the middle of the window
    age = (t_first - t_audio0[0]) - t_in_audio
    print(f"first audio after the reconnect has {freq:.0f} Hz, so it is about {age:.2f} s old")
    print("RESULT:", "OK, bounded delay" if age < 1.0 and reconnect - stall_start < 4.0 else "TOO SLOW or too old")


if __name__ == "__main__":
    main()
