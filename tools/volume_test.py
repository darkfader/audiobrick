#!/usr/bin/env python3
"""Do the Windows volume keys / mixer slider / mute change the sound at the Audio Brick?  (microphone in front of a speaker)

  python tools/volume_test.py

Needs tools/brick_sender.py running and pycaw (pip install pycaw). Plays a 600 Hz tone into VB-Cable's playback device, sets that device's
Windows volume to 100 %, 50 %, 25 %, 10 % and then mute, and measures the 600 Hz level at the speaker for each. VB-Cable itself ignores
Windows' volume; the sender follows it (see VolumeFollower in brick_sender.py). Your original volume and mute state are restored.
"""
import time
import warnings

import numpy as np
import sounddevice as sd

warnings.filterwarnings("ignore")
from pycaw.pycaw import AudioUtilities  # noqa: E402

FS = 48000


def find(prefix, kind):
    api = sd.query_hostapis()
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    for i, d in enumerate(sd.query_devices()):
        if d[key] > 0 and d["name"].startswith(prefix) and "WASAPI" in api[d["hostapi"]]["name"]:
            return i
    raise SystemExit(f"device '{prefix}' not found")


def main():
    vol = next(d.EndpointVolume for d in AudioUtilities.GetAllDevices()
               if d.FriendlyName and d.FriendlyName.startswith("Speakers (VB-Audio Virtual Cable"))
    orig_scalar, orig_mute = vol.GetMasterVolumeLevelScalar(), vol.GetMute()
    cable, mic = find("Speakers (VB-Audio Virtual Cable", "out"), find("Microphone (Sennheiser Profile", "in")
    n = [0]

    def cb(out, frames, t, s):
        ph = (np.arange(frames) + n[0]) / FS
        n[0] += frames
        out[:, 0] = out[:, 1] = 0.3 * np.sin(2 * np.pi * 600 * ph)

    rec = []
    ins = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32",
                         callback=lambda d, f, t, s: rec.append((time.time(), d[:, 0].copy())))
    ins.start()
    results = []
    try:
        with sd.OutputStream(samplerate=FS, channels=2, device=cable, dtype="float32", callback=cb):
            vol.SetMute(0, None)
            for label, scalar, mute in (("100 %", 1.0, 0), ("50 %", 0.5, 0), ("25 %", 0.25, 0), ("10 %", 0.1, 0), ("muted", 1.0, 1)):
                vol.SetMasterVolumeLevelScalar(scalar, None)
                vol.SetMute(mute, None)
                time.sleep(2.5)  # sender connects (first time), settles
                t0 = time.time()
                time.sleep(1.5)
                x = np.concatenate([d for tt, d in rec if tt > t0])
                spec = np.abs(np.fft.rfft(x[: FS] * np.hanning(FS)))
                f = np.fft.rfftfreq(FS, 1 / FS)
                band = spec[(f > 580) & (f < 620)].max()
                results.append((label, 20 * np.log10(band + 1e-9)))
    finally:
        vol.SetMasterVolumeLevelScalar(orig_scalar, None)
        vol.SetMute(orig_mute, None)
        ins.stop()
        ins.close()
    ref = results[0][1]
    print(f"{'Windows volume':<16}{'level at speaker (600 Hz)':>26}")
    for label, db in results:
        print(f"{label:<16}{db - ref:>20.1f} dB re 100 %")


if __name__ == "__main__":
    main()
