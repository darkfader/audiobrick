#!/usr/bin/env python3
"""Background sender: whatever Windows plays to VB-Cable's playback device comes out of the Audio Brick.

  pythonw tools/brick_sender.py [host]            # hidden, no window (see tools/Install-BrickSender.ps1 for autostart)
  python  tools/brick_sender.py [host] --verbose  # with a console, prints what it does

Windows apps play to ONE device ("Speakers (VB-Audio Virtual Cable)", the cable's playback side). This program records the
cable's other end ("CABLE Output") and sends it to the Brick on TCP port 4010 (protocol in main/stream.h). It only holds a
connection while there is sound: after SILENCE_S seconds of silence it disconnects, so the Brick's amp can mute, go Hi-Z and
power down, and it reconnects as soon as sound starts (about 0.2 s of pre-buffering at the Brick). No ffmpeg needed.

Volume: VB-Cable ignores Windows' volume and mute, and carries no volume information. So the sender follows the cable device's
Windows volume (media keys, mixer slider) and sets the Brick's own amp volume to match (POST /volume?level=0..1, from -70 dB up to
the speaker profile's cap), which keeps the full 16-bit resolution at low volumes. Mute silences the stream on the PC. To stay safe
the Brick's volume goes UP in small steps only (at most 0.05 of the range, about 2 dB, every 0.25 s) and never jumps; down is
immediate. At start the Windows slider is set to the Brick's current level instead of the other way round, and changes made on
the Brick's web page are copied back to the slider. Needs pycaw (pip install pycaw), otherwise the volume is not followed.

The Brick's address defaults to audiobrick.local. The password is read from the AUDIOBRICK_PASSWORD environment variable or from
%APPDATA%/audiobrick/password (one line). Only one copy runs at a time.
"""
import argparse
import json
import os
import queue
import socket
import sys
import threading
import time
import urllib.request
from pathlib import Path

import numpy as np
import sounddevice as sd

FS = 48000
SILENCE_S = 5.0        # disconnect after this long without sound
THRESHOLD = 0.0005     # peak (full scale 1.0) above which a block counts as sound, about -66 dBFS
UP_STEP = 0.05         # largest increase of the Brick's volume per request (fraction of its -70 dB .. cap range, about 2 dB)
UP_INTERVAL_S = 0.25
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


class VolumeFollower(threading.Thread):
    """Windows volume slider -> Brick amp volume; Windows mute -> silence on the PC. See the module docstring."""

    def __init__(self, host, pw):
        super().__init__(daemon=True)
        self.host, self.pw = host, pw
        self.muted = False

    def brick(self, path, method="GET"):
        ip = socket.gethostbyname(self.host)
        req = urllib.request.Request(f"http://{ip}{path}", data=b"" if method == "POST" else None,
                                     headers={"X-Token": self.pw}, method=method)
        with urllib.request.urlopen(req, timeout=4) as r:
            return json.loads(r.read())

    def brick_level(self):
        return float(self.brick("/status")["volume_level"])

    def run(self):
        try:
            import warnings
            import comtypes
            warnings.filterwarnings("ignore")
            from pycaw.pycaw import AudioUtilities
        except ImportError:
            log("pycaw is not installed: Windows volume keys will not change the volume (pip install pycaw)")
            return
        comtypes.CoInitialize()
        vol = None
        sent = None            # the Brick level we last set or saw
        last_win = None        # the Windows slider position we last set or saw
        last_push = 0.0
        last_poll = 0.0
        while True:
            try:
                if vol is None:
                    found = None
                    for d in AudioUtilities.GetAllDevices():
                        n = d.FriendlyName or ""
                        if "VB-Audio Virtual Cable" in n and not n.startswith("CABLE Output"):
                            found = d.EndpointVolume
                            break
                    if found is None:
                        time.sleep(5)
                        continue
                    sent = self.brick_level()                    # raises if the Brick is not reachable: try again later
                    found.SetMasterVolumeLevelScalar(sent, None)  # the slider starts at the Brick's level, never the other way round
                    last_win = float(found.GetMasterVolumeLevelScalar())
                    vol = found                                   # only now: everything above worked
                    log(f"following the Windows volume (Brick is at {sent:.2f})")
                self.muted = bool(vol.GetMute())
                now = time.time()
                win = float(vol.GetMasterVolumeLevelScalar())
                if abs(win - last_win) > 0.004:                       # the user moved the Windows slider
                    last_win = win
                if abs(last_win - sent) > 0.004 and now - last_push >= UP_INTERVAL_S:
                    target = min(last_win, sent + UP_STEP)            # up in small steps, down at once
                    sent = float(self.brick(f"/volume?level={target:.3f}", "POST").get("vol_db") is not None and target)
                    last_push = now
                elif abs(last_win - sent) <= 0.004 and now - last_poll > 5.0:
                    last_poll = now                                    # changed on the Brick's own page? copy it to the slider
                    cur = self.brick_level()
                    if abs(cur - sent) > 0.04:  # the Brick works in whole dB (0.026 of the range)
                        sent = cur
                        vol.SetMasterVolumeLevelScalar(cur, None)
                        last_win = float(vol.GetMasterVolumeLevelScalar())
                time.sleep(0.1)
            except Exception as e:
                log(f"volume follower: {type(e).__name__}: {e}")
                if vol is not None and "COM" in type(e).__name__:
                    vol = None
                time.sleep(3)


def find_cable_output():
    api = sd.query_hostapis()
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0 and "CABLE Output" in d["name"] and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    return None


def connect(host, pw):
    ip = socket.gethostbyname(host)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # Windows auto-tunes the send buffer up to megabytes, which would hold seconds of audio when the Brick is slow or stalled
    # (that shows up as audio lag). 32 KB is about 170 ms of 48 kHz stereo 16-bit.
    s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 32768)
    s.settimeout(5)
    s.connect((ip, 4010))
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
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    # A send that does not complete within 1 s means the Brick stopped reading (rebooting, crashed, cable pulled): give up on
    # this connection and reconnect, instead of blocking for minutes while the cable's buffer backs up behind us.
    s.settimeout(1.0)
    return s


def run(host):
    pw = password()
    if not pw:
        log("no password: set AUDIOBRICK_PASSWORD or write it to %APPDATA%/audiobrick/password")
        return 2
    # Live audio must never build up delay: keep at most about 300 ms (15 blocks of 20 ms) and drop the OLDEST when behind.
    # An unbounded or long queue was the cause of "enormous video/audio lag" when the Brick rebooted during a firmware update.
    q = queue.Queue(maxsize=15)

    def cb(data, frames, t, status):
        while True:
            try:
                q.put_nowait(data.copy())
                return
            except queue.Full:
                try:
                    q.get_nowait()
                except queue.Empty:
                    pass

    def flush():
        n = 0
        while True:
            try:
                q.get_nowait()
                n += 1
            except queue.Empty:
                return n

    vf = VolumeFollower(host, pw)
    vf.start()
    prev_gain = 1.0
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
            if block is not None:
                g = 0.0 if vf.muted else 1.0  # Windows mute; ramped within the block to avoid clicks
                block = block * np.linspace(prev_gain, g, len(block), dtype="float32")[:, None]
                prev_gain = g
            if block is not None and float(np.max(np.abs(block))) > THRESHOLD:
                last_loud = now
            if sock is None and now - last_loud < 1.0 and block is not None:
                sock = connect(host, pw)
                flush()  # whatever piled up while connecting is old: start from the present
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
            time.sleep(1)
            flush()  # never replay audio that is seconds old
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
