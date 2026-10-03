#!/usr/bin/env python3
"""Send a short, quiet test tone to the Audio Brick with the VBAN or Scream protocol.

  python tools/netaudio_test.py HOST vban                  # VBAN, 48 kHz, 16 bit, stereo, stream name "Test1"
  python tools/netaudio_test.py HOST vban --rate 44100 --bits 24 --channels 1
  python tools/netaudio_test.py HOST scream                # Scream unicast to HOST:4010
  python tools/netaudio_test.py HOST scream --multicast    # Scream to 239.255.77.77:4010

The receivers must be switched on first (web page, "Network audio", or POST /netaudio).
This is only a stand-in for Voicemeeter / the Scream driver when testing the board.
"""
import argparse
import math
import socket
import struct
import time

VBAN_RATES = [6000, 12000, 24000, 48000, 96000, 192000, 384000, 8000, 16000, 32000, 64000, 128000, 256000, 512000,
              11025, 22050, 44100, 88200, 176400, 352800, 705600]


def pcm(samples_l, samples_r, bits):
    out = bytearray()
    if samples_r is None:  # mono
        samples_r = [None] * len(samples_l)
    for l, r in zip(samples_l, samples_r):
        for v in (l, r) if r is not None else (l,):
            iv = int(v * ((1 << (bits - 1)) - 1))
            out += iv.to_bytes(bits // 8, "little", signed=True)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("kind", choices=["vban", "scream"])
    ap.add_argument("--rate", type=int, default=48000)
    ap.add_argument("--bits", type=int, default=16, choices=[16, 24, 32])
    ap.add_argument("--channels", type=int, default=2, choices=[1, 2])
    ap.add_argument("--seconds", type=float, default=3.0)
    ap.add_argument("--freq", type=float, default=880.0)
    ap.add_argument("--level", type=float, default=0.03, help="amplitude 0..1 (default 0.03, about -30 dBFS)")
    ap.add_argument("--name", default="Test1")
    ap.add_argument("--multicast", action="store_true")
    ap.add_argument("--port", type=int)
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.kind == "vban":
        dest = (args.host, args.port or 6980)
        per_packet = 256
    else:
        dest = ("239.255.77.77" if args.multicast else args.host, args.port or 4010)
        sock.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        per_packet = 1152 // (args.bits // 8 * args.channels)

    total = int(args.seconds * args.rate)
    start = time.perf_counter()
    sent = 0
    frame_counter = 0
    while sent < total:
        n = min(per_packet, total - sent)
        t = [(sent + i) / args.rate for i in range(n)]
        s = [args.level * math.sin(2 * math.pi * args.freq * x) for x in t]
        body = pcm(s, s if args.channels == 2 else None, args.bits)
        if args.kind == "vban":
            sr = VBAN_RATES.index(args.rate)
            fmt = {16: 1, 24: 2, 32: 3}[args.bits]
            hdr = b"VBAN" + bytes([sr, n - 1, args.channels - 1, fmt]) + args.name.encode().ljust(16, b"\0")[:16] + struct.pack("<I", frame_counter)
            sock.sendto(hdr + body, dest)
        else:
            base = 0x80 if args.rate % 44100 == 0 else 0x00
            mult = args.rate // (44100 if base else 48000)
            hdr = bytes([base | mult, args.bits, args.channels]) + struct.pack("<H", 0x0003 if args.channels == 2 else 0x0004)
            sock.sendto(hdr + body, dest)
        frame_counter += 1
        sent += n
        # real-time pacing
        delay = start + sent / args.rate - time.perf_counter()
        if delay > 0:
            time.sleep(delay)
    print(f"sent {frame_counter} packets ({args.seconds:.1f} s) to {dest[0]}:{dest[1]}")


if __name__ == "__main__":
    main()
