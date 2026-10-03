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

## Loops

| File | What it does |
|---|---|
| `check_loop.py` | Decodes a clip as the board plays it, repeats it twice and measures the join (level step, tone change, click, dip). Only clips that pass are allowed to be called `*_loop.mp3`. |
| `make_loop.py` | Cuts a seamlessly looping clip out of a longer recording of a steady sound (finds the best matching moments, equal-power crossfade, normalises, then runs the check). |

## Network audio, radio and synthesizer tests (the board must have the feature switched on)

| File | What it does |
|---|---|
| `netaudio_test.py` | Sends a quiet test tone with the VBAN or Scream protocol (a stand-in for Voicemeeter / the Scream driver). |
| `listen_test.py` | Runs `netaudio_test.py` while recording with the microphone and checks that the right pitch comes out of the speaker. |
| `osc_test.py` | Builds real OSC messages and a bundle for the synthesizer and checks the notes by microphone. |
| `cable_test.py` | Plays a tone into the VB-Cable, sends it to the board with `stream.py`, and checks by microphone that it comes out of the speaker. |
| `latency_test.py` | Measures command-to-sound latency (and with `--boot` the start-up time after a reset). |

## Amp power-down and Voicemeeter (v1.1.0)

- `amp_power_test.py [host]` - sets short idle times, watches the amp go Hi-Z and off, and checks the 2-20 kHz band around each transition for clicks (microphone in front of a speaker; keep the room quiet).
- `wake_test.py [host] [--path stream|tone] [--repeat N]` - when does a sound become audible from awake, Hi-Z and powered-off, and does it last as long as it should? Result 2026-10-03: no measurable wake cost on either path (powering the chip up takes about 30 ms and happens while the mixer's 100 ms fade-in is still at zero), tone length unchanged.
- `voicemeeter_vban.py` / `voicemeeter_test.py` - drive Voicemeeter's VBAN out stream from the command line and test the Windows -> Voicemeeter -> Brick path (not working yet, see CLAUDE.md).
- `sine_via_voicemeeter.py [host] [--freq 440] [--db -24] [--seconds N]` - steady phase-continuous sine from the PC through Voicemeeter and VBAN to the Brick (a single monotone for buffer tests). Measured 2026-10-03: buffer 148-167 ms, 0 underruns, 0 dropped packets, amplitude wobble at the speaker about 11 % peak-to-peak (slow room effect; the Brick's own tone shows 30 %).
  **Lesson:** Voicemeeter's first strip is the PC microphone, routed to the speakers and to the VBAN bus by default. That fed the speaker sound back and made a +-80 % wobble at 2.6 and 9 Hz (it was not the Brick's buffering). `voicemeeter_vban.py` now mutes the three hardware strips when it sets the stream up (`--keep-inputs` to skip).
- `brick_sender.py [host]` - background sender: plays whatever Windows apps send to VB-Cable's playback device ("Speakers (VB-Audio Virtual Cable)") on the Brick over TCP 4010. Connects only while there is sound (disconnects after 5 s of silence, so the amp can power down). No ffmpeg. Password from `AUDIOBRICK_PASSWORD` or `%APPDATA%/audiobrick/password`. One copy at a time.
- `Install-BrickSender.ps1` - installs `brick_sender.py` as a hidden logon task (normal user), `-Remove` undoes it. `sender_test.py` - tests it with the microphone (pitch, stream released after silence).
- `Set-SingleBrickAudioDevice.ps1` (administrator) - hides VB-Cable's second 16-channel playback device so Windows lists exactly one; `-Restore` shows it again. Uses the audio endpoint API (`IPolicyConfig::SetEndpointVisibility`); `Disable-PnpDevice` does nothing for audio endpoints. **Do not use it to hide Voicemeeter's devices**: hiding any VB-Audio device made Voicemeeter go silent on the test PC.
- `voicemeeter_vban.py` also starts Voicemeeter hidden when it is not running, mutes the hardware strips and hides the window (`--show-window`, `--keep-inputs` to opt out). Voicemeeter itself was uninstalled again on the development PC (about 18 devices); see docs/windows-setup.md.
