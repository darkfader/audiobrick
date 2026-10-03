#!/usr/bin/env python3
"""End-to-end test of "Windows sound -> virtual audio cable -> Audio Brick".

  python tools/cable_test.py HOST

Plays a tone into the VB-Audio Virtual Cable's playback device ("CABLE Input", or "Speakers (VB-Audio Virtual Cable)" on
some installs; the device Windows apps would be
set to), starts tools/stream.py capturing the cable's recording side ("CABLE Output") and sending it to the board,
and records the speaker with the microphone to check the pitch. Needs ffmpeg, the VB-Cable driver and the password
in AUDIOBRICK_PASSWORD.
"""
import os
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd

HOST = sys.argv[1]
FREQ = 700.0
FS = 48000


def find(name_part, kind):
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    hostapis = sd.query_hostapis()
    cands = [(i, d) for i, d in enumerate(sd.query_devices()) if d[key] > 0 and name_part.lower() in d["name"].lower()]
    cands.sort(key=lambda c: 0 if "WASAPI" in hostapis[c[1]["hostapi"]]["name"] or "MME" in hostapis[c[1]["hostapi"]]["name"] else 1)
    return cands[0][0] if cands else None


def main():
    cable_in = find("Speakers (VB-Audio", "out") or find("CABLE Input", "out")
    mic = find("Sennheiser Profile", "in")
    if cable_in is None or mic is None:
        sys.exit("need the VB-Cable playback device and the microphone")
    stream_py = Path(__file__).with_name("stream.py")
    env = dict(os.environ)
    sender = subprocess.Popen([sys.executable, str(stream_py), HOST, "--device", "CABLE Output (VB-Audio Virtual Cable)"],
                              env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    time.sleep(3.0)  # let ffmpeg open the capture device and the board take the stream
    # sd.play() would cancel a recording started with sd.rec() (they share one global stream), so use real streams
    chunks = []
    mic_stream = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32", callback=lambda d, f, t, st: chunks.append(d.copy()))
    mic_stream.start()
    time.sleep(0.5)
    t = np.arange(int(4.0 * FS)) / FS
    tone = (0.5 * np.sin(2 * np.pi * FREQ * t)).astype("float32")
    out_stream = sd.OutputStream(samplerate=FS, channels=1, device=cable_in, dtype="float32")
    out_stream.start()
    out_stream.write(tone)
    time.sleep(1.5)
    out_stream.stop(); out_stream.close()
    mic_stream.stop(); mic_stream.close()
    rec = np.concatenate(chunks) if chunks else np.zeros((FS, 1), dtype="float32")
    sender.terminate()
    try:
        out, _ = sender.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        sender.kill()
        out = ""
    x = rec[:, 0]
    noise = float(np.sqrt(np.mean(x[: int(0.4 * FS)] ** 2)))
    win = FS
    energies = [np.sqrt(np.mean(x[i:i + win] ** 2)) for i in range(0, len(x) - win, FS // 10)]
    k = int(np.argmax(energies)) * (FS // 10)
    seg = x[k:k + win] * np.hanning(win)
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    f = np.fft.rfftfreq(1 << 17, 1 / FS)
    sel = f > 100
    peak = f[sel][int(np.argmax(spec[sel]))]
    lvl = 20 * np.log10(float(max(energies)) + 1e-12)
    nz = 20 * np.log10(noise + 1e-12)
    print(f"stream.py said: {' | '.join(l.strip() for l in (out or '').splitlines() if l.strip())[:200]}")
    print(f"loudest frequency at the speaker {peak:.1f} Hz (played {FREQ:.0f} Hz); {lvl:.1f} dBFS, {lvl - nz:.0f} dB above the room noise")
    print("RESULT:", "OK, Windows sound through the virtual cable came out of the Brick" if abs(peak - FREQ) < 5 and lvl - nz > 12 else "NOT HEARD (or wrong pitch)")


if __name__ == "__main__":
    main()
