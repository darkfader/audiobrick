#!/usr/bin/env python3
"""Send audio to the Audio Brick over TCP (port 4010).

  python tools/stream.py HOST song.mp3
  python tools/stream.py HOST --device "CABLE Output (VB-Audio Virtual Cable)"   # Windows system audio via a virtual cable

The password comes from the AUDIOBRICK_PASSWORD environment variable, or is asked for.
Needs ffmpeg on the PATH. The board's own volume cap (speaker profile) still applies.
"""
import argparse
import getpass
import os
import socket
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("host")
    ap.add_argument("source", nargs="?", help="audio file or URL (anything ffmpeg can read)")
    ap.add_argument("--device", help="Windows DirectShow audio device to capture instead of a file")
    ap.add_argument("--port", type=int, default=4010)
    args = ap.parse_args()
    if not args.source and not args.device:
        ap.error("give a file or --device")

    password = os.environ.get("AUDIOBRICK_PASSWORD") or getpass.getpass("Audio Brick password: ")

    cmd = ["ffmpeg", "-hide_banner", "-loglevel", "error"]
    cmd += ["-f", "dshow", "-i", f"audio={args.device}"] if args.device else ["-i", args.source]
    cmd += ["-vn", "-f", "s16le", "-ar", "48000", "-ac", "2", "pipe:1"]

    sock = socket.create_connection((args.host, args.port), timeout=10)
    sock.sendall(f"STREAM {password} 48000 2\n".encode())
    reply = b""
    while not reply.endswith(b"\n"):
        chunk = sock.recv(1)
        if not chunk:
            break
        reply += chunk
    if reply.strip() != b"OK":
        sys.exit(f"board refused the stream: {reply.decode(errors='replace').strip() or 'no answer'}")
    sock.settimeout(None)

    print("streaming… press Ctrl+C to stop")
    ff = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    try:
        while True:
            data = ff.stdout.read(4096)
            if not data:
                break
            sock.sendall(data)  # blocks when the board's buffer is full, which paces file playback
    except (KeyboardInterrupt, BrokenPipeError, ConnectionError):
        pass
    finally:
        ff.terminate()
        sock.close()


if __name__ == "__main__":
    main()
