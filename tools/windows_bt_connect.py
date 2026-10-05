#!/usr/bin/env python3
"""Connect (or disconnect) this Windows PC's Bluetooth audio to the paired Audio Brick from the command line, like the "Connect" button.

  python tools/windows_bt_connect.py [--name "Audio Brick"] [--disconnect]

Windows connects to speakers and headphones itself (the speaker cannot start the audio link), so this asks Windows to do it: it switches the
device's A2DP audio-sink service off and on again with BluetoothSetServiceState (bthprops.cpl). The device must already be paired
(tools/windows_bt_pair.py). The Brick address is read from the Windows registry list of paired Bluetooth devices.
After a successful connect a new playback device named like "Headphones (Audio Brick Stereo)" appears in Windows' sound settings.
"""
import argparse
import ctypes
import ctypes.wintypes as wt
import subprocess
import sys
import time
import winreg

A2DP_SINK = "{0000110B-0000-1000-8000-00805F9B34FB}"


class GUID(ctypes.Structure):
    _fields_ = [("a", ctypes.c_ulong), ("b", ctypes.c_ushort), ("c", ctypes.c_ushort), ("d", ctypes.c_ubyte * 8)]


class SYSTEMTIME(ctypes.Structure):
    _fields_ = [(n, ctypes.c_ushort) for n in ("y", "mo", "dow", "d", "h", "mi", "s", "ms")]


class BLUETOOTH_DEVICE_INFO(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("Address", ctypes.c_ulonglong), ("ulClassofDevice", wt.ULONG),
                ("fConnected", wt.BOOL), ("fRemembered", wt.BOOL), ("fAuthenticated", wt.BOOL),
                ("stLastSeen", SYSTEMTIME), ("stLastUsed", SYSTEMTIME), ("szName", ctypes.c_wchar * 248)]


def guid(s):
    g = GUID()
    ctypes.windll.ole32.CLSIDFromString(s, ctypes.byref(g))
    return g


def paired_address(name):
    """Look through the paired devices Windows keeps in its registry (HKLM\\SYSTEM\\CurrentControlSet\\Services\\BTHPORT\\Parameters\\Devices)."""
    key = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices")
    for i in range(winreg.QueryInfoKey(key)[0]):
        addr = winreg.EnumKey(key, i)
        try:
            k = winreg.OpenKey(key, addr)
            raw, _ = winreg.QueryValueEx(k, "Name")
            if bytes(raw).rstrip(b"\x00").decode("utf-8", "replace") == name:
                return int(addr, 16)
        except OSError:
            continue
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", default="Audio Brick")
    ap.add_argument("--disconnect", action="store_true")
    a = ap.parse_args()
    addr = paired_address(a.name)
    if addr is None:
        sys.exit(f"'{a.name}' is not in Windows' paired Bluetooth list (pair it first with tools/windows_bt_pair.py)")
    dev = BLUETOOTH_DEVICE_INFO()
    dev.dwSize = ctypes.sizeof(dev)
    dev.Address = addr
    bt = ctypes.WinDLL("bthprops.cpl")
    rc = bt.BluetoothGetDeviceInfo(None, ctypes.byref(dev))
    if rc != 0:
        sys.exit(f"BluetoothGetDeviceInfo failed ({rc})")
    print(f"{dev.szName}  {addr:012x}  connected={bool(dev.fConnected)}  remembered={bool(dev.fRemembered)}")
    g = guid(A2DP_SINK)
    if a.disconnect:
        print("disable audio sink service:", bt.BluetoothSetServiceState(None, ctypes.byref(dev), ctypes.byref(g), 0))
        return
    # off, then on: Windows then sets up the A2DP link to the speaker. Right after pairing Windows is still installing the audio service
    # for the device and answers 87 (invalid parameter) or 1168 (not found); try again for up to about 40 s.
    for attempt in range(1, 9):
        r_off = bt.BluetoothSetServiceState(None, ctypes.byref(dev), ctypes.byref(g), 0)
        time.sleep(1)
        r_on = bt.BluetoothSetServiceState(None, ctypes.byref(dev), ctypes.byref(g), 1)
        print(f"attempt {attempt}: disable {r_off}, enable {r_on} (0 = ok)")
        if r_on == 0:
            break
        time.sleep(4)
    time.sleep(3)
    bt.BluetoothGetDeviceInfo(None, ctypes.byref(dev))
    print("connected now:", bool(dev.fConnected), "- the audio playback device is ready when sounddevice lists 'Headphones (Audio Brick...)'")


if __name__ == "__main__":
    main()
