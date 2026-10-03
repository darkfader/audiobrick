# Sending Windows sound to the Audio Brick

The Brick can receive sound from a PC in four ways. Pick by what you want to see in Windows. All links below were checked on
2026-10-03.

| Way | Windows sees | Needs installing | Works on Windows 11 | Status on this PC |
|---|---|---|---|---|
| **A. Virtual cable + `tools/stream.py`** (TCP, port 4010) | a normal playback device ("VB-Audio Virtual Cable") | VB-Cable, ffmpeg, Python | yes (signed driver) | **installed and tested end to end** |
| **B. Voicemeeter + VBAN** (UDP 6980) | Voicemeeter's virtual devices; the Brick is an outgoing VBAN stream | Voicemeeter (Banana is enough) | yes (signed driver) | **installed and tested end to end** |
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

## A. Virtual cable + stream.py (works today)

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
