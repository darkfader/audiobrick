#!/usr/bin/env python3
"""Where does the delay of "Windows app -> VB-Cable -> brick_sender -> Audio Brick -> speaker" come from?

  python tools/cable_latency_test.py [host] [--mic]

1. PC chain: plays short clicks into VB-Cable's playback device and records "CABLE Output" at the same time on this PC
   (both timestamped with the same clock), which is what the app -> cable -> sender capture part costs.
2. Brick: reads the Brick's stream buffer (status.media.buffer_ms), the configured pre-buffer (GET /latency) and adds the fixed
   output stage (I2S DMA 6 x 256 frames = 32 ms plus one 5.3 ms mixer block).
3. With --mic: the same clicks are also recorded with the microphone at the speaker; the click-to-sound time is reported as seen
   (it includes the mic's own input delay of about 40-50 ms, so use differences between latency settings, not the absolute value).

Needs brick_sender.py running (installed task) and AUDIOBRICK_PASSWORD only for changing the setting, not for this test.
"""
import argparse
import json
import socket
import sys
import time
import urllib.request

import numpy as np
import sounddevice as sd

FS = 48000


def find(prefix, kind):
    api = sd.query_hostapis()
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    for i, d in enumerate(sd.query_devices()):
        if d[key] > 0 and d["name"].startswith(prefix) and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    sys.exit(f"device '{prefix}' not found")


def get(host, path):
    with urllib.request.urlopen(f"http://{host}{path}", timeout=4) as r:
        return json.loads(r.read())


def click_train(n_clicks=8, gap=1.0):
    """1.5 s of a quiet tone (opens the stream and lets the buffer settle), then a 150 ms burst of 600 Hz every `gap` seconds,
    starting abruptly (a short click is too weak for the microphone)."""
    total = int((1.5 + n_clicks * gap + 1.0) * FS)
    x = np.zeros(total, dtype="float32")
    t = np.arange(total) / FS
    x[: int(1.5 * FS)] = 0.05 * np.sin(2 * np.pi * 300 * t[: int(1.5 * FS)])
    starts = []
    for k in range(n_clicks):
        s = int((1.5 + k * gap) * FS)
        n = int(0.15 * FS)
        x[s:s + n] = 0.5 * np.sin(2 * np.pi * 600 * np.arange(n) / FS)
        starts.append(s)
    return x, starts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="audiobrick.local")
    ap.add_argument("--mic", action="store_true")
    a = ap.parse_args()
    host = socket.gethostbyname(a.host)
    cable_out = find("Speakers (VB-Audio Virtual Cable", "out")
    cable_in = find("CABLE Output", "in")
    mic = find("Microphone (Sennheiser Profile", "in") if a.mic else None

    x, starts = click_train()
    pos = [0]
    play_t0 = [None]
    cap, mic_chunks = [], []

    def out_cb(out, frames, t, s):
        if play_t0[0] is None:
            play_t0[0] = time.perf_counter()
        chunk = x[pos[0]:pos[0] + frames]
        out[:len(chunk), 0] = out[:len(chunk), 1] = chunk
        if len(chunk) < frames:
            out[len(chunk):] = 0
        # the first sample of this block reaches the device roughly one block later; the same offset applies to all clicks
        pos[0] += frames

    def make_in_cb(store):
        return lambda d, f, t, s: store.append((time.perf_counter(), d[:, 0].copy()))

    ins = sd.InputStream(samplerate=FS, channels=2, device=cable_in, dtype="float32", blocksize=480,
                         callback=lambda d, f, t, s: cap.append((time.perf_counter(), d[:, 0].copy())),
                         extra_settings=sd.WasapiSettings(auto_convert=True))
    mic_stream = None
    if mic is not None:
        mic_stream = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32", callback=make_in_cb(mic_chunks))
        mic_stream.start()
    ins.start()
    time.sleep(0.3)
    with sd.OutputStream(samplerate=FS, channels=2, device=cable_out, dtype="float32", callback=out_cb):
        time.sleep(len(x) / FS + 0.5)
        status = get(host, "/status")["media"]
        try:
            lat = get(host, "/latency")
        except Exception:
            lat = {}
    ins.stop(); ins.close()
    if mic_stream:
        mic_stream.stop(); mic_stream.close()

    def onsets(chunks, relative=False):
        from scipy.signal import butter, sosfiltfilt
        t = np.concatenate([np.full(len(d), tt) - (len(d) - np.arange(len(d))) / FS for tt, d in chunks])  # timestamp of each sample
        v = np.concatenate([d for _, d in chunks]).astype("float64")
        band = sosfiltfilt(butter(4, [520, 680], btype="band", fs=FS, output="sos"), v)
        env = np.convolve(np.abs(band), np.ones(96) / 96, mode="same")
        found = []
        for k in range(len(starts)):
            lo = play_t0[0] + 1.5 + k * 1.0 - 0.1
            hi = lo + 0.9
            sel = np.where((t > lo) & (t < hi))[0]
            if len(sel) == 0:
                found.append(None)
                continue
            w = env[sel]
            floor = float(np.median(env[(t > lo - 0.3) & (t < lo)])) if np.any((t > lo - 0.3) & (t < lo)) else 0.0
            peak = float(w.max())
            if peak < 4 * max(floor, 1e-6):
                found.append(None)       # nothing clearly above the room noise
                continue
            found.append(t[sel[np.argmax(w > 0.5 * peak)]])
        return found

    pc = [None if f is None else (f - (play_t0[0] + 1.5 + k * 1.0)) * 1000 for k, f in enumerate(onsets(cap))]
    pc = [v for v in pc if v is not None]
    print(f"PC chain (app -> cable -> capture), {len(pc)} clicks: median {np.median(pc):.0f} ms (min {min(pc):.0f}, max {max(pc):.0f})")
    print(f"Brick: pre-buffer setting {lat.get('prebuffer_ms', '170 (fixed, no /latency yet)')} ms, buffer level now {status['buffer_ms']} ms,"
          f" output stage about 37 ms")
    est = np.median(pc) + status["buffer_ms"] + 37
    print(f"estimated click-to-sound at the speaker: about {est:.0f} ms (PC chain + buffer + output stage; network time is about 1 ms)")
    if mic is not None and mic_chunks:
        m = [None if f is None else (f - (play_t0[0] + 1.5 + k * 1.0)) * 1000 for k, f in enumerate(onsets(mic_chunks, relative=True))]
        m = [v for v in m if v is not None]
        if m:
            print(f"as heard by the microphone: median {np.median(m):.0f} ms (includes the mic's own ~45 ms)")


if __name__ == "__main__":
    main()
