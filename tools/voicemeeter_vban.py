#!/usr/bin/env python3
"""Configure Voicemeeter's first outgoing VBAN stream by script, through Voicemeeter's own Remote API.

  python tools/voicemeeter_vban.py 192.168.2.40 --name Brick --route 3      # send route 3 to the Brick
  python tools/voicemeeter_vban.py --off                                    # switch the stream off
  python tools/voicemeeter_vban.py --show                                   # print the current setting

Voicemeeter (Banana) must be running. The same settings can be made by hand in Voicemeeter's VBAN window (Menu -> VBAN).
--route is Voicemeeter's source number for the stream (which strip or bus feeds it); use --show after changing it in the
window to see the number your layout uses.
Parameters follow VB-Audio's Voicemeeter Remote API (vban.outstream[n].on / name / ip / port / sr / channel / bit / quality / route).
"""
import argparse
import ctypes
import sys
import time

DLL = r"C:\Program Files (x86)\VB\Voicemeeter\VoicemeeterRemote64.dll"


def api():
    vm = ctypes.WinDLL(DLL)
    vm.VBVMR_Login.restype = ctypes.c_long
    vm.VBVMR_Logout.restype = ctypes.c_long
    vm.VBVMR_SetParameters.argtypes = [ctypes.c_char_p]
    vm.VBVMR_SetParameters.restype = ctypes.c_long
    vm.VBVMR_GetParameterFloat.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
    vm.VBVMR_GetParameterFloat.restype = ctypes.c_long
    vm.VBVMR_GetParameterStringA.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    vm.VBVMR_GetParameterStringA.restype = ctypes.c_long
    vm.VBVMR_IsParametersDirty.restype = ctypes.c_long
    return vm


def get_f(vm, name):
    v = ctypes.c_float()
    return v.value if vm.VBVMR_GetParameterFloat(name.encode(), ctypes.byref(v)) == 0 else None


def get_s(vm, name):
    buf = ctypes.create_string_buffer(512)
    return buf.value.decode(errors="replace") if vm.VBVMR_GetParameterStringA(name.encode(), buf) == 0 else None


def show(vm, n=0):
    vm.VBVMR_IsParametersDirty()
    time.sleep(0.2)
    p = f"vban.outstream[{n}]"
    print("VBAN enabled :", get_f(vm, "vban.Enable"))
    for key in ("on", "port", "sr", "channel", "bit", "quality", "route"):
        print(f"  {key:<8}:", get_f(vm, f"{p}.{key}"))
    for key in ("name", "ip"):
        print(f"  {key:<8}:", get_s(vm, f"{p}.{key}"))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ip", nargs="?", help="the Audio Brick's address")
    ap.add_argument("--name", default="Brick")
    ap.add_argument("--route", type=int, default=0)
    ap.add_argument("--port", type=int, default=6980)
    ap.add_argument("--stream", type=int, default=0, help="which of Voicemeeter's outgoing VBAN streams (0-7)")
    ap.add_argument("--keep-inputs", action="store_true",
                    help="do not mute Voicemeeter's hardware input strips (see below)")
    ap.add_argument("--off", action="store_true")
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    vm = api()
    rc = vm.VBVMR_Login()
    if rc < 0:
        sys.exit(f"cannot reach Voicemeeter (login returned {rc}); is it running?")
    time.sleep(1.0)  # the API needs a moment after login before parameters can be set or read
    try:
        p = f"vban.outstream[{args.stream}]"
        if args.show:
            show(vm, args.stream)
            return
        if args.off:
            script = f"{p}.on=0;"
        else:
            if not args.ip:
                sys.exit("give the Brick's address, e.g.  python tools/voicemeeter_vban.py 192.168.2.40")
            # 48 kHz, 2 channels, 16 bit (bit=1), no compression
            script = (f"vban.Enable=1;{p}.on=0;{p}.name=\"{args.name}\";{p}.ip=\"{args.ip}\";{p}.port={args.port};"
                      f"{p}.sr=48000;{p}.channel=2;{p}.bit=1;{p}.quality=0;{p}.route={args.route};")
        r = vm.VBVMR_SetParameters(script.encode())
        print("set parameters:", "ok" if r == 0 else f"returned {r}")
        time.sleep(0.7)
        if not args.off:
            if not args.keep_inputs:
                # Voicemeeter's first strip is the PC microphone, routed to the speakers AND the VBAN bus by default. That
                # sends the microphone to the Brick and, with the PC speakers on, makes a feedback loop that wobbles the
                # sound by +-80 % (measured). Mute the three hardware strips; the virtual inputs are what we send.
                vm.VBVMR_SetParameters(b"Strip[0].Mute=1;Strip[1].Mute=1;Strip[2].Mute=1;")
                print("hardware input strips muted (use --keep-inputs to leave them alone)")
            r = vm.VBVMR_SetParameters(f"{p}.on=1;".encode())  # ignored when sent in the same batch as the settings
            print("switch on:", "ok" if r == 0 else f"returned {r}")
            time.sleep(0.5)
        show(vm, args.stream)
    finally:
        vm.VBVMR_Logout()


if __name__ == "__main__":
    main()
