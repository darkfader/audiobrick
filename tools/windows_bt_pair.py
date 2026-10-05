#!/usr/bin/env python3
"""Pair this Windows PC with the Audio Brick's Bluetooth audio from the command line (no Settings clicks).

  uv run --python 3.12 --with winrt-runtime --with winrt-Windows.Foundation --with winrt-Windows.Foundation.Collections ^
         --with winrt-Windows.Devices.Enumeration --with winrt-Windows.Devices.Bluetooth tools/windows_bt_pair.py [host] [--name "Audio Brick"] [--unpair]

What it does: opens the Brick's pairing window (POST /bluetooth/pair, needs AUDIOBRICK_PASSWORD), scans for the Brick with Windows' own
Bluetooth API, pairs with "confirm" pairing (answering the confirmation itself) and prints the result. With --unpair it removes the pairing
from Windows again. Afterwards Windows (or the Brick, which connects back to its paired device) makes the audio connection; check
GET /bluetooth for "connected". Python 3.12 is used because the winrt packages do not support the newest Python yet.
"""
import argparse
import asyncio
import os
import socket
import sys
import urllib.request

from winrt.windows.devices.bluetooth import BluetoothDevice
from winrt.windows.devices.enumeration import DeviceInformation, DevicePairingKinds, DevicePairingProtectionLevel


def brick(host, path):
    ip = socket.gethostbyname(host)
    req = urllib.request.Request(f"http://{ip}{path}", data=b"", headers={"X-Token": os.environ.get("AUDIOBRICK_PASSWORD", "")}, method="POST")
    return urllib.request.urlopen(req, timeout=8).read().decode()


async def find(name, paired):
    """Paired devices come from a plain query; unpaired ones need a live Bluetooth search (a device watcher), since a plain query
    only returns what Windows already knows."""
    sel = BluetoothDevice.get_device_selector_from_pairing_state(paired)
    if paired:
        infos = await DeviceInformation.find_all_async_aqs_filter(sel)
        return next((i for i in infos if i.name == name), None), [i.name for i in infos]
    found = {}
    watcher = DeviceInformation.create_watcher_aqs_filter(sel)

    def added(w, info):
        found[info.id] = info

    watcher.add_added(added)
    watcher.start()
    for _ in range(30):                       # up to about 30 s
        await asyncio.sleep(1)
        if any(i.name == name for i in found.values()):
            break
    watcher.stop()
    return next((i for i in found.values() if i.name == name), None), [i.name for i in found.values()]


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host", nargs="?", default="audiobrick.local")
    ap.add_argument("--name", default="Audio Brick")
    ap.add_argument("--unpair", action="store_true")
    a = ap.parse_args()

    if a.unpair:
        info, _ = await find(a.name, True)
        if not info:
            print("not paired with Windows")
            return
        res = await info.pairing.unpair_async()
        print("unpair:", res.status)
        return

    info, seen = await find(a.name, True)
    if info:
        print(f"'{a.name}' is already paired with Windows (device id {info.id[-17:]})")
        return
    if os.environ.get("AUDIOBRICK_PASSWORD"):
        print("pairing window:", brick(a.host, "/bluetooth/pair?on=1").strip()[:90])
    for attempt in range(1, 3):
        info, seen = await find(a.name, False)
        print(f"scan {attempt}: {len(seen)} unpaired Bluetooth devices seen: {seen[:12]}")
        if info:
            break
    if not info:
        sys.exit(f"'{a.name}' not found by Windows. Is the pairing window open (page: Pair a new device)?")
    custom = info.pairing.custom

    def requested(sender, args):
        print("Windows asks to pair, kind:", args.pairing_kind, "-> accepting")
        if args.pairing_kind == DevicePairingKinds.PROVIDE_PIN:   # legacy pairing (the Brick with SSP switched off): the PIN is 0000
            args.accept_with_pin("0000")
        else:
            args.accept()

    custom.add_pairing_requested(requested)
    res = await custom.pair_async(DevicePairingKinds.CONFIRM_ONLY | DevicePairingKinds.PROVIDE_PIN)
    print("pairing result:", res.status)


if __name__ == "__main__":
    asyncio.run(main())
