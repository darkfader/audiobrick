# Tools

PC-side helpers for the Esparagus Audio Brick (run with the system Python, not the ESP-IDF one:
the IDF environment has no numpy/sounddevice).

| File | What it does |
|---|---|
| `stream.py` | Sends an audio file, URL or a Windows capture device to the board over TCP port 4010. Needs `ffmpeg`. Password from `AUDIOBRICK_PASSWORD` or a prompt. |
| `measure.py` | Plays a quiet log sweep through one speaker and records it with a USB microphone, then plots the response. Silent check by default; `--play` plays the sweep; `--analyse WAV` re-processes a recording. Needs `numpy scipy sounddevice matplotlib`. |
| `mic_sennheiser_profile_0deg.csv` | On-axis response of the Sennheiser Profile microphone (digitised from the manufacturer's spec sheet, typical unit, about ±2 dB). `measure.py` subtracts it from the result. |

Examples (PowerShell):

```powershell
$env:AUDIOBRICK_PASSWORD = '<web password>'
python tools/stream.py 192.168.2.40 song.mp3
python tools/stream.py 192.168.2.40 --device "CABLE Output (VB-Audio Virtual Cable)"
python tools/measure.py 192.168.2.40 --mic "Sennheiser Profile"                 # silent level check
python tools/measure.py 192.168.2.40 --mic "Sennheiser Profile" --play --channel left --label ns-b40
```

Measurement tips: put the speaker on something soft and cover the desk between speaker and mic, keep the mic
at the listening distance and aimed at the speaker, repeat 3 times and average. A bare desk adds a strong
reflection (a notch around 3 kHz at 6 cm path difference) that is not part of the speaker.
Recordings made with firmware older than 0.6.2 contain a 440 Hz beep after the sweep; ignore them.
