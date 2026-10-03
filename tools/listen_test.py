#!/usr/bin/env python3
"""Check by microphone that a network-audio test really comes out of the speaker at the right pitch.

  python tools/listen_test.py HOST vban --rate 44100 --bits 24 --channels 1 --freq 660
  python tools/listen_test.py HOST scream --multicast --freq 880

Records with the PC microphone while tools/netaudio_test.py sends a tone, then reports the loudest frequency
and the level against the room noise. Uses a level of 0.5 (-6 dBFS) so it is picked up; the amp volume cap applies.
"""
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd


def main():
    args = sys.argv[1:]
    freq = 880.0
    if "--freq" in args:
        freq = float(args[args.index("--freq") + 1])
    mic = "Sennheiser Profile"
    dev = [i for i, d in enumerate(sd.query_devices()) if d["max_input_channels"] > 0 and mic in d["name"]][0]
    fs = 48000
    secs = 3.0
    buf = sd.rec(int((secs + 3.5) * fs), samplerate=fs, channels=1, device=dev, dtype="float32")
    time.sleep(1.0)
    sender = Path(__file__).with_name("netaudio_test.py")
    subprocess.run([sys.executable, str(sender)] + args + ["--seconds", str(secs), "--level", "0.5"], check=True)
    sd.wait()
    x = buf[:, 0]
    noise = float(np.sqrt(np.mean(x[: int(0.8 * fs)] ** 2)))
    # find the loudest 1 s window
    win = fs
    energies = [np.sqrt(np.mean(x[i:i + win] ** 2)) for i in range(0, len(x) - win, fs // 10)]
    k = int(np.argmax(energies)) * (fs // 10)
    seg = x[k:k + win] * np.hanning(win)
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    freqs = np.fft.rfftfreq(1 << 17, 1 / fs)
    peak = freqs[int(np.argmax(spec[(freqs > 100)]) + np.argmax(freqs > 100))]
    level = 20 * np.log10(float(energies[k // (fs // 10)]) + 1e-12)
    print(f"loudest frequency {peak:.1f} Hz (sent {freq:.1f} Hz); level {level:.1f} dBFS vs room noise "
          f"{20 * np.log10(noise + 1e-12):.1f} dBFS -> {level - 20 * np.log10(noise + 1e-12):.0f} dB above the noise")
    ok = abs(peak - freq) < 4 and level - 20 * np.log10(noise + 1e-12) > 12
    print("RESULT:", "OK, the tone came out of the speaker" if ok else "NOT HEARD (or wrong pitch)")


if __name__ == "__main__":
    main()
