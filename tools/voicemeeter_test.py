#!/usr/bin/env python3
"""End-to-end test of "Windows app -> Voicemeeter -> VBAN -> Audio Brick", driven entirely from the command line.

  python tools/voicemeeter_test.py 192.168.2.40                 # try routes 0-9, report which one reaches the speaker
  python tools/voicemeeter_test.py 192.168.2.40 --route 3       # test one route
  python tools/voicemeeter_test.py 192.168.2.40 --cable         # feed Voicemeeter from VB-Cable instead of its own virtual input

For each route it configures Voicemeeter's first outgoing VBAN stream (tools/voicemeeter_vban.py), plays a tone into
Voicemeeter's virtual input ("Voicemeeter Input", the device an app would use), and records the speaker with the
microphone. Needs Voicemeeter Banana running, the board's VBAN receiver switched on, and the microphone in front of a speaker.
"""
import argparse
import subprocess
import sys
import time
from pathlib import Path

import numpy as np
import sounddevice as sd

FS = 48000
FREQ = 600.0
HERE = Path(__file__).parent


def find(name_part, kind):
    key = "max_output_channels" if kind == "out" else "max_input_channels"
    api = sd.query_hostapis()
    c = [(i, d) for i, d in enumerate(sd.query_devices())
         if d[key] > 0 and name_part.lower() in d["name"].lower() and "WASAPI" in api[d["hostapi"]]["name"]]
    return c[0][0] if c else None


def use_cable():
    """Point Voicemeeter's first hardware input at VB-Cable's recording side and route it to A2, A3, B1, B2 (not A1: silent on the PC)."""
    sys.path.insert(0, str(HERE))
    import voicemeeter_vban as v
    vm = v.api()
    vm.VBVMR_Login()
    time.sleep(1.0)
    vm.VBVMR_SetParameters(b'Strip[0].device.wdm="CABLE Output (VB-Audio Virtual Cable)";')
    time.sleep(2.0)
    vm.VBVMR_SetParameters(b"Strip[0].A1=0;Strip[0].A2=1;Strip[0].A3=1;Strip[0].B1=1;Strip[0].B2=1;Strip[0].Mute=0;")
    time.sleep(0.5)
    vm.VBVMR_Logout()


def vban(*a):
    return subprocess.run([sys.executable, str(HERE / "voicemeeter_vban.py"), *a], capture_output=True, text=True)


def try_route(host, route, vm_in, mic):
    vban(host, "--name", "Brick", "--route", str(route))
    time.sleep(1.0)
    chunks = []
    ins = sd.InputStream(samplerate=FS, channels=1, device=mic, dtype="float32", callback=lambda d, f, t, s: chunks.append(d.copy()))
    ins.start()
    time.sleep(0.6)
    t = np.arange(int(2.5 * FS)) / FS
    tone = (0.5 * np.sin(2 * np.pi * FREQ * t)).astype("float32")
    out = sd.OutputStream(samplerate=FS, channels=2, device=vm_in, dtype="float32")
    out.start()
    out.write(np.column_stack([tone, tone]))
    time.sleep(1.2)
    out.stop(); out.close()
    ins.stop(); ins.close()
    x = np.concatenate(chunks)[:, 0] if chunks else np.zeros(FS, dtype="float32")
    noise = float(np.sqrt(np.mean(x[: int(0.5 * FS)] ** 2))) + 1e-9
    win = FS
    best, bk = 0.0, 0
    for i in range(0, len(x) - win, FS // 10):
        e = float(np.sqrt(np.mean(x[i:i + win] ** 2)))
        if e > best:
            best, bk = e, i
    seg = x[bk:bk + win] * np.hanning(win)
    spec = np.abs(np.fft.rfft(seg, 1 << 17))
    f = np.fft.rfftfreq(1 << 17, 1 / FS)
    sel = f > 100
    peak = float(f[sel][int(np.argmax(spec[sel]))])
    above = 20 * np.log10(best / noise)
    heard = abs(peak - FREQ) < 6 and above > 12
    return heard, peak, above


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--route", type=int)
    ap.add_argument("--cable", action="store_true", help="use VB-Cable as the source")
    args = ap.parse_args()
    if args.cable:
        use_cable()
    vm_in = find("Speakers (VB-Audio Virtual Cable)" if args.cable else "Voicemeeter Input", "out")
    mic = find("Sennheiser Profile", "in")
    if vm_in is None or mic is None:
        sys.exit("need Voicemeeter's virtual input device and the microphone")
    found = None
    for route in ([args.route] if args.route is not None else range(10)):
        heard, peak, above = try_route(args.host, route, vm_in, mic)
        print(f"route {route}: {'HEARD' if heard else 'silent'}  (loudest {peak:.0f} Hz, {above:.0f} dB above the room noise)")
        if heard and found is None:
            found = route
            if args.route is None:
                break
    vban(args.host, "--off") if found is None else None
    print("RESULT:", f"OK, route {found} carries a Windows app's sound to the Brick" if found is not None else "no route reached the speaker")


if __name__ == "__main__":
    main()
