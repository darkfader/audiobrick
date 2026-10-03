#!/usr/bin/env python3
"""Background sender: whatever Windows plays to VB-Cable's playback device comes out of the Audio Brick.

  pythonw tools/brick_sender.py [host]            # hidden, no window (see tools/Install-BrickSender.ps1 for autostart)
  python  tools/brick_sender.py [host] --verbose  # with a console, prints what it does

Windows apps play to ONE device ("Speakers (VB-Audio Virtual Cable)", the cable's playback side). This program records the
cable's other end ("CABLE Output") and sends it to the Brick on TCP port 4010 (protocol in main/stream.h). It only holds a
connection while there is sound: after SILENCE_S seconds of silence it disconnects, so the Brick's amp can mute, go Hi-Z and
power down, and it reconnects as soon as sound starts (about 0.2 s of pre-buffering at the Brick). No ffmpeg needed.

The Brick's address defaults to audiobrick.local. The password is read from the AUDIOBRICK_PASSWORD environment variable or from
%APPDATA%/audiobrick/password (one line). Only one copy runs at a time.
"""
import argparse
import os
import queue
import socket
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd

FS = 48000
SILENCE_S = 5.0        # disconnect after this long without sound
THRESHOLD = 0.0005     # peak (full scale 1.0) above which a block counts as sound, about -66 dBFS
LOG = Path(os.environ.get("TEMP", ".")) / "brick_sender.log"
verbose = False


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    if verbose:
        print(line, flush=True)
    try:
        with LOG.open("a", encoding="utf-8") as f:
            f.write(line + "\n")
        if LOG.stat().st_size > 200_000:
            LOG.write_text(line + "\n", encoding="utf-8")
    except OSError:
        pass


def password():
    pw = os.environ.get("AUDIOBRICK_PASSWORD", "").strip()
    if not pw:
        f = Path(os.environ.get("APPDATA", ".")) / "audiobrick" / "password"
        if f.exists():
            pw = f.read_text(encoding="utf-8").strip()
    return pw


def find_cable_output():
    api = sd.query_hostapis()
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and "CABLE Output" in d["name"] and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    return None


def connect(host, pw):
    ip = socket.gethostbyname(host)
    s = socket.create_connection((ip, 4010), timeout=5)
    s.sendall(f"STREAM {pw} {FS} 2\n".encode())
    reply = b""
    while not reply.endswith(b"\n"):
        c = s.recv(1)
        if not c:
            break
        reply += c
    if reply.strip() != b"OK":
        s.close()
        raise ConnectionError(f"Brick refused the stream: {reply.decode(errors='replace').strip() or 'no answer'}")
    s.settimeout(None)
    return s


def run(host):
    pw = password()
    if not pw:
        log("no password: set AUDIOBRICK_PASSWORD or write it to %APPDATA%/audiobrick/password")
        return 2
    q = queue.Queue(maxsize=400)

    def cb(data, frames, t, status):
        try:
            q.put_nowait(data.copy())
        except queue.Full:
            pass  # the sender is stuck; drop rather than grow

    sock = None
    last_loud = 0.0
    dev = None
    stream = None
    while True:
        try:
            if stream is None:
                dev = find_cable_output()
                if dev is None:
                    log("CABLE Output not found (is VB-Cable installed and enabled?); retrying")
                    time.sleep(10)
                    continue
                stream = sd.InputStream(samplerate=FS, channels=2, device=dev, dtype="float32", blocksize=960,
                                        callback=cb, extra_settings=sd.WasapiSettings(auto_convert=True))
                stream.start()
                log("capturing CABLE Output")
            try:
                block = q.get(timeout=1.0)
            except queue.Empty:
                block = None
            now = time.time()
            if block is not None and float(np.max(np.abs(block))) > THRESHOLD:
                last_loud = now
            if sock is None and now - last_loud < 1.0 and block is not None:
                sock = connect(host, pw)
                log("connected to the Brick")
            if sock is not None:
                if block is not None:
                    pcm = (np.clip(block, -1.0, 1.0) * 32767.0).astype("<i2").tobytes()
                    sock.sendall(pcm)
                if now - last_loud > SILENCE_S:
                    sock.close()
                    sock = None
                    log("silence, disconnected")
        except (OSError, ConnectionError) as e:
            log(f"{type(e).__name__}: {e}")
            if sock is not None:
                try:
                    sock.close()
                except OSError:
                    pass
                sock = None
            if isinstance(e, sd.PortAudioError) or stream is not None and not stream.active:
                try:
                    stream.close()
                except Exception:
                    pass
                stream = None
            time.sleep(3)
        except sd.PortAudioError as e:
            log(f"audio device problem: {e}")
            stream = None
            time.sleep(5)


def main():
    global verbose
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host", nargs="?", default="audiobrick.local")
    ap.add_argument("--verbose", action="store_true")
    a = ap.parse_args()
    verbose = a.verbose
    guard = socket.socket()
    try:  # one copy only: a second one would send the same sound twice
        guard.bind(("127.0.0.1", 47113))
    except OSError:
        log("already running")
        return 1
    try:
        return run(a.host)
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
