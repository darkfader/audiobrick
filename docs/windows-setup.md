# Sending Windows sound to the Audio Brick

The Brick can receive sound from a PC in four ways. Pick by what you want to see in Windows. All links below were checked on
2026-10-03.

| Way | Windows sees | Needs installing | Works on Windows 11 | Status on this PC |
|---|---|---|---|---|
| **A. VB-Cable + hidden sender `tools/brick_sender.py`** (TCP, port 4010) | ONE playback device ("Speakers (VB-Audio Virtual Cable)") | VB-Cable, Python | yes (signed driver) | **installed and tested end to end; recommended** |
| **B. Voicemeeter + VBAN** (UDP 6980) | about 18 Voicemeeter devices; the Brick is an outgoing VBAN stream | Voicemeeter (Banana is enough) | yes (signed driver) | worked end to end, then **uninstalled again** (too many devices) |
| **C. Scream** (UDP 4010, unicast or multicast) | a playback device "Speakers (Scream)" | the Scream driver | **no, not normally (see below)** | not installed, not recommended |
| **D. Bluetooth** | a normal Bluetooth speaker | nothing | yes | not built into the firmware |

The firmware side of B and C is built and tested (with a stand-in sender that speaks the same protocols, see
`tools/netaudio_test.py`). A and B have been tested with the real Windows software; C has not (no usable driver).

## Downloads

| What | Link | Notes |
|---|---|---|
| VB-Audio Virtual Cable (VB-Cable) | https://vb-audio.com/Cable/ | free (donationware). Installed here as "Speakers (VB-Audio Virtual Cable)" / "CABLE Output". |
| Voicemeeter (Standard, Banana, Potato) | https://vb-audio.com/Voicemeeter/ | Banana: https://vb-audio.com/Voicemeeter/banana.htm |
| VBAN (what Voicemeeter's network streams use) | https://vb-audio.com/Voicemeeter/vban.htm | specification: https://vb-audio.com/Voicemeeter/VBANProtocol_Specifications.pdf |
| Scream (official, release 4.0, 2022-09-17) | https://github.com/duncanthrax/scream/releases/tag/4.0 | see "Scream on Windows 11" below before installing |
| Scream project page and README | https://github.com/duncanthrax/scream | |
| ffmpeg for Windows | https://www.gyan.dev/ffmpeg/builds/ (other builds: https://ffmpeg.org/download.html) | must be on the PATH; `tools/stream.py` calls it |
| Python | https://www.python.org/downloads/ | for the scripts in `tools/` |
| Python packages for the tools | `pip install numpy scipy sounddevice matplotlib pyserial` | `stream.py` needs only ffmpeg and Python |
| ESP-IDF 5.5 (to build the firmware) | https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/index.html | |

## A. One device: VB-Cable and the hidden background sender (recommended)

This gives Windows exactly **one** playback device, "Speakers (VB-Audio Virtual Cable)" (that is how VB-Cable's "CABLE Input" is named on current
installs), plus one recording device that only the sender uses. No window, no ffmpeg. Tested end to end (599.9 Hz at the speaker).

1. Install VB-Cable (link above) and Python with `pip install numpy sounddevice pycaw` (pycaw lets the sender follow the Windows volume keys).
2. Hide VB-Cable's second, 16-channel playback device (as administrator; nothing is uninstalled, `-Restore` shows it again):
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools/Set-SingleBrickAudioDevice.ps1
   ```
3. Install the background sender (normal user, no administrator). It asks for the Brick's web password once and stores it in
   `%APPDATA%\audiobrick\password`, then starts `tools/brick_sender.py` hidden at every logon:
   ```powershell
   powershell -ExecutionPolicy Bypass -File tools/Install-BrickSender.ps1
   ```
   (`-HostName 192.168.2.40` if `audiobrick.local` does not resolve on your network; `-Remove` uninstalls it again.)
4. In Windows' sound settings, play the app (or the default output) to "Speakers (VB-Audio Virtual Cable)". Your normal speakers stay silent for that app.

**Volume keys:** VB-Cable ignores Windows' volume and mute completely (measured: the level at the cable output does not change at all), and it carries no
volume information. So the sender reads the Windows volume of "Speakers (VB-Audio Virtual Cable)" and sets the **Brick's own amp volume** to match
(this keeps the full 16-bit resolution at low volumes); mute silences the stream. The media keys act on the *default* playback device, so make the
cable the default device in Windows' sound settings if you want the keys to control the Brick. Safety: the Brick's volume only goes up in small steps (about 2 dB
every 0.25 s) and never jumps; the speaker profile's cap still applies. At start the Windows slider is set to the Brick's current level (not the other
way round), and changes made on the Brick's web page are copied back to the slider. Check it with `python tools/volume_test.py`.

How it behaves: the sender listens to the cable and connects to the Brick only while there is sound; 5 s after the last sound it disconnects again, so the
amp can mute, go Hi-Z and power down. When sound starts it reconnects (about 0.2 s of pre-buffering). Log: `%TEMP%\brick_sender.log`.
Test it with the microphone: `python tools/sender_test.py`.

### Manual alternative: stream.py (ffmpeg)

The older way, also good for files and URLs:

1. Install VB-Cable (link above) and ffmpeg.
2. In Windows, set the app you want to hear on the Brick (or the whole system) to play to the cable's playback device
   ("Speakers (VB-Audio Virtual Cable)" or "CABLE Input"). Note this silences your normal speakers for that app.
3. Run (PowerShell):
   ```powershell
   $env:AUDIOBRICK_PASSWORD = '<the Brick web password>'
   python tools/stream.py 192.168.2.40 --device "CABLE Output (VB-Audio Virtual Cable)"
   ```
4. Stop with Ctrl+C. To also hear it on the PC, use Voicemeeter or Windows' "Listen to this device" on the cable's recording side.
`tools/cable_test.py` automates a check of this chain (tone into the cable, microphone at the speaker).

## B. Voicemeeter and VBAN

1. Install Voicemeeter Banana (link above). Its drivers are signed by Microsoft, so no special Windows settings are needed. The installer asks for a restart; on this PC restarting the Windows Audio service instead (`Restart-Service audiosrv -Force` in an administrator PowerShell) and then closing and reopening Voicemeeter was enough. Until then Voicemeeter showed no level for any sound and sent nothing.
2. On the Brick's web page, open "Network audio from Windows", tick **VBAN receiver**, optionally enter your PC's address in
   "Only accept audio from this address", and Save.
3. In Voicemeeter: open the **VBAN** window (menu), add an outgoing stream, set **IP address** to the Brick's address, **port 6980**, a stream
   name (any, or the one you set on the Brick), format 48 kHz 16 bit stereo, tick the stream on, and choose the strip/bus to send.
   From the command line: `python tools/voicemeeter_vban.py audiobrick.local --name Brick --route 0` (address `192.168.2.40` also works; `--off` switches the stream off; `tools/voicemeeter_test.py <address>` plays a tone into "Voicemeeter Input" and checks with a microphone that it arrives). Voicemeeter's strips go to A1/B1 by default; select "Voicemeeter Input" as an app's output device.
4. Sound from that bus now plays on the Brick. The Brick plays the stream only while packets arrive and releases the main channel when they stop.

### Voicemeeter clutter (read before choosing B)

Voicemeeter Banana adds about 18 audio devices to Windows (8 playback, 8 recording, plus the cable ones if VB-Cable is installed). Trying to hide the
extra ones did **not** work on the PC this was developed on: after hiding any VB-Audio device (even only the VB-Cable ones) Voicemeeter received no
audio from "Voicemeeter Input" until everything was shown again and the Windows Audio service restarted. If you want exactly one device in Windows, use
route A ("one device, hidden background sender") instead.

Other things learned with Voicemeeter:
- Its first strip is your **PC microphone**, routed to the speakers and to the VBAN stream. That sends the microphone to the Brick and makes a feedback
  loop that wobbles every sound by about +-80 % (measured). `tools/voicemeeter_vban.py` mutes the three hardware strips when it sets the stream up.
- Voicemeeter is the audio engine, there is no background service: its program must be running (it can sit hidden in the tray; `tools/voicemeeter_vban.py`
  starts it hidden and hides the window, which may flash for a moment).
- The Brick ignores a sender that only sends digital silence (Voicemeeter's VBAN stream never stops): after 4 s it releases the channel so the amp can idle.

## C. Scream on Windows 11 (read this first)

The official Scream 4.0 driver is signed with a certificate that **expired on 2023-07-07** and carries no timestamp (checked with
`Get-AuthenticodeSignature`: "a required certificate is not within its validity period"). The author states there will be no new signature
(issue #215 in the Scream repository), and none of the forks has a release. On current Windows 11 the project's own instructions
(its README section "Installation on Windows 11", and issues #206, #215, #227, #242) need all of the following:

- Secure Boot switched **off** in the BIOS,
- Windows **Test Mode** (`bcdedit /set testsigning on`, reboot), which weakens a Windows protection and **stops Easy Anti-Cheat (VRChat) from starting while it is on**,
- installing with `pnputil` and then running the installer batch file to create the device, then turning Test Mode off again,
- and reports say it can break again after Windows updates.

That is why Scream is not installed here, and why the firmware is built **without** the Scream receiver by default. To include it anyway, run `idf.py menuconfig`, open *Audio Brick features*, tick *Scream receiver*, and rebuild (or build with `sdkconfig.defaults.full`). If you decide to do it anyway, do it yourself following the README at
https://github.com/duncanthrax/scream and Microsoft's explanation of Test Mode at
https://learn.microsoft.com/en-us/windows-hardware/drivers/install/test-signing. To uninstall, remove the device in Device Manager with
"Delete the driver software for this device". On the Brick: tick **Scream receiver** (and "also listen for multicast" if you use the default
multicast address); in the driver's settings choose unicast and the Brick's address, or leave multicast.

## Linux or macOS?

Scream also has receivers/senders for Linux, and VBAN has clients for most systems; the Brick's receivers only care about the packets.
`tools/netaudio_test.py` shows the exact packet layouts for both.
