# Esparagus Audio Brick (ESP32 variant) - test bench

Sonocotta Esparagus Audio Brick, **ESP32 (not S3)** revision, received by mistake; the S3 replacement is expected around early 2027. Keep firmware portable between the two, since pins differ (see table).

Upstream: https://github.com/sonocotta/esparagus-media-center (hardware in `/hardware`, firmware in `/firmware`, ESPHome configs in `/firmware/esphome/5-audio-brick/`).

## Hardware

- MCU: ESP32-D0WD-V3 rev v3.1, 8 MB PSRAM. **Flash is 16 MB (verified with esptool)**; the upstream YAML's 8 MB is wrong. MAC 20:9b:a9:6f:f0:b4.
- Serial: CH340 on COM5 (this PC). Auto-reset into download mode does NOT work. Hold BOOT (the button farther from the board edge), tap EN (the button nearer the edge), release BOOT after esptool connects.
- DAC/amp: TI TAS5825M (I2S in, I2C control, class-D BTL out, DSP with 15-band EQ, hardware volume, fault reporting).
- Ethernet: W5500 over SPI. Preferred transport for this project. Wi-Fi is the fallback.
- Bluetooth: classic A2DP works on this ESP32 only, not on the S3. Optional, lower priority.
- USB-C: CH340 serial for flashing and logs.
- Status LED: one WS2812.
- No OLED fitted (optional SPI SSD1306, not owned). Skip or disable display code and the `oled.yaml` ESPHome package. Use the WS2812 and serial logs for status.
- Power: 5-26 V DC (chip max 26.4 V), up to 10 A. **Connector on the owner's board is unverified.** Upstream rev C1 schematic: DC barrel jack DC-044A-A250 (pin 1 = VDD, pins 2/5 = GND, so probably center-positive) in parallel with a 2-pin 5.08 mm terminal CN4 (CN5 unused). Crowd Supply and Elecrow list a 4-pin power connector, so the shipped board may be a newer revision than C1 (the repo has no newer files than C1/D as of 2026-10-03). Check the board and verify polarity with a multimeter.
- Reverse polarity (rev C1): D3 (M7, about 1 A, 30 A surge) sits across VDD/GND with the cathode on VDD. It is a shunt "crowbar": it conducts on reverse polarity and shorts the supply. There is no fuse, TVS or series protection on the board. Use a current-limited or fused supply, 24 V or less. Not verified for the owner's revision.
- Output: 4-pin snap-in speaker connector, bridged (BTL). **Never tie either output terminal to ground.** 4-8 ohm loads.
  - 12 V: about 2x10 W into 4 ohm.
  - 24 V: about 2x30 W into 8 ohm, or 65 W bridged mono into 4 ohm.

## Pinout

| Function | ESP32 (this board) | ESP32-S3 |
|---|---|---|
| I2S BCK | 26 | 14 |
| I2S WS | 25 | 15 |
| I2S DOUT | 22 | 16 |
| I2C SDA | 21 | 8 |
| I2C SCL | 27 | 9 |
| DAC PWDN/enable | 33 | ? |
| DAC FAULTZ (speaker fault) | 39 | 18 |
| DAC WARNZ | 36 | 4 |
| WS2812 LED | 12 | 21 |
| SPI SCLK/MOSI/MISO | 18/23/19 | 12/11/13 |
| W5500 CS/INT/RST | 5/35/14 | ? |
| OLED CS/DC/RST (unused, no display) | 15/4/32 | ? |

TAS5825M I2C address is **0x4C** (verified by I2C scan; the only device on the bus). FAULTZ/WARNZ read 1 (no fault) at idle. Only 4 MB of the 8 MB PSRAM is mapped by default on the ESP32. Take the ESP32-S3 `?` entries from the upstream S3 config once the S3 board arrives.

## Toolchain

- Flash and logs: USB-C, CH340 on COM5. Use the manual BOOT/EN sequence above before flashing.
- Schematic (upstream rev C1, `hardware/5-esparagus-audio-brick/rev-c1/*-schematic.pdf`; board has no revision print; assumed to be C1, the latest single-DAC ESP32 revision) shows a standard CH340C auto-reset circuit: DTR/RTS -> two S8050 transistors -> EN and GPIO0. Buttons: SW4 = GPIO0 (BOOT), SW3 = MCU_RST (EN). So auto-reset should exist in hardware, yet esptool reports "Wrong boot mode 0x13". Cause not found yet.
- Flashing recipe: user enters download mode by hand, then run from `build/`: `python -m esptool --chip esp32 -p COM5 -b 460800 --before no_reset --after hard_reset write_flash "@flash_args"`. `idf.py flash` resets via RTS, which fails here ("Wrong boot mode 0x13").
- ESP-IDF v5.5.3 is installed at `C:\Users\darkf\git\EspHidEmulation\esp-idf`. Activate with `. C:\Users\darkf\git\EspHidEmulation\esp-idf\export.ps1`, then `idf.py`.
- USB-C powers only the ESP32/CH340 (enough for flashing and bring-up with no speakers). The amp stage needs the DC input (12 V recommended); USB 5 V limits it to about 5 W.
- Prebuilt streaming firmware:
  - Squeezelite-ESP32 (LMS, Spotify Connect, AirPlay).
  - Snapclient (multi-room sync).
  - ESPHome media player (Home Assistant).
  - Upstream builds with PlatformIO: `pio run -e esparagus-audio-brick -t upload`, then `-t uploadfs`.
- Custom firmware (synth, OSC): **ESP-IDF** (`idf.py`), C or C++. No Arduino. Not PlatformIO (its IDF support lags) and not Zephyr (no TAS5825M driver, less mature ESP32 Wi-Fi/BT/PSRAM support).
  - Required bring-up order:
    1. Pull PWDN (GPIO33) high.
    2. Init I2C.
    3. Configure the TAS5825M (I2S format, volume, unmute) over I2C.
    4. Start I2S.
  - Poll FAULTZ and WARNZ and mute on fault.

## Current firmware (v1.1.0, ESP-IDF, in this folder)

2026-10-05 (late): **Sleep timer / quiet hours / alarm** (`sources/schedule.c`, `AB_FEATURE_SCHEDULE`, page card, `/schedule*`, see schedule.h): the sleep timer stops all sources and drops the Bluetooth device; quiet hours hold the amp under a ceiling via `dac_set_quiet_cap()` while the wanted volume (`s_vol_db`, saved) is kept; the alarm loops a stored clip at the current volume for `alarm_s` seconds. Tested on the Brick: ceiling -40 dB applied and the -21 dB volume came back; alarm test and sleep timer set/cancel; the real alarm time and a full sleep run are not tested. **Wi-Fi limit (2026-10-06, see docs/wifi.md)**: with Wi-Fi on and a Bluetooth stream playing, a clip mixed on top makes the music stutter (25 to ~200 dropouts per clip; none with Wi-Fi off); cause not found (suspected external-RAM bus contention; ruled out memory, PVDD, flash reads, core sharing). Wi-Fi was switched off on the Brick and the page warns about it. The build is now `-O2` (was `-Og`; the audio block used 3.9 of 5.3 ms idle), decoder work buffers and stacks are in PSRAM, decoders are pinned to core 1, and `GET /diag/audio` (login) shows worst-case audio block times. **Wi-Fi memory (2026-10-06)**: Wi-Fi on costs about 50 KB of internal RAM (free 82 KB -> 32 KB), which stopped clips (28 KB stack needs a contiguous block) and pushed radio to 3 KB free. Fix: the MP3 decoder task stacks (clips, radio) now live in PSRAM (`xTaskCreateWithCaps`, `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`). **A PSRAM-stack task must never touch flash** (IDF asserts `esp_task_stack_is_sane_cache_disabled`, crash with reboot): so `clip_play_slot` reads a clip (up to 1.5 MB) into PSRAM in the caller's task and the decoder plays from memory; bigger files keep an internal stack. Measured with Wi-Fi + Bluetooth stack on: idle free 53 KB, two clip decoders 24 KB, radio min 12 KB, a flash write (clip upload) during two decoders is fine, repeated plays do not leak. Dynamic Wi-Fi TX buffers are not selectable here (needs `SPIRAM_IGNORE_NOTFOUND`, which conflicts with BSS in PSRAM). Wi-Fi works against a real access point ("Touwa", 192.168.2.43). The Wi-Fi checkbox now works with no stored network (so you can scan first). `/media/stop` stops radio too (there is no `/radio/stop`). Not yet tested: Bluetooth connected and streaming together with Wi-Fi. **Wi-Fi client** (`net/wifi.c`, `AB_FEATURE_WIFI`, page card, `/wifi*` endpoints, see wifi.h): up to 8 remembered networks in NVS, auto-join of the strongest, Ethernet + Wi-Fi both DHCP, wired preferred; off until a network is added; `net_has_ip()` is true for either interface. Not yet tested with a real access point. **Partition table changed**: OTA slots are now 3 MB (image 2.1 MB), storage 0x620000 (9.9 MB); needed one serial flash (BOOT/EN, then tap EN again after the flash because the automatic reset does not work) and the clips were re-uploaded from `clips/`. Also: Bluetooth Disconnect button (`POST /bluetooth/disconnect`, opening a pairing window drops the current device), the amp volume is saved in NVS (`vol_db`), sources moved into folders (`docs/architecture.md`), every optional component is a Kconfig switch. Flashing over the network during playback is allowed by the owner (the music pauses about 10 s).

Later in v1.1.x: a clip played from the web page (or `/clips/play`) while a live stream (PC sound via the cable, VBAN, radio) owns the main channel is mixed on top through the mixer's one-shot channel (`SLOT_EVENT`) instead of being refused (`clips_http.c`; Stop ends it too; mic test: the 440 Hz stream tone stays, -2 dB from the 0.75 mix scale, while the clip adds +6 dB in 500 Hz-5 kHz). `/status` has `est_peak` (loudest possible peak at the current volume) and the page's Estimated loudness card shows it when no test tone is on. Page: the amp protection chips now live in the Status card, the percentage of the speaker-profile limit sits under the Estimated loudness bar, and a login-only 'Amp power saving' card edits `/power` (Hi-Z and power-down delays). `/status.media.overlay` names a clip mixed over a stream. Access audit (2026-10-04, live, no token): all 27 state-changing POSTs answer 401 except `/logout`; GETs for status, profile, session and the page are public on purpose (logged-out page, Home Assistant), the other GETs (settings, clip names) are readable on the LAN without login. Latency (2026-10-04, `tools/cable_latency_test.py --mic`): the delay of PC sound over the cable is the PC chain (app -> WASAPI -> VB-Cable -> capture, about 65-80 ms) plus the Brick's stream buffer (about equal to the pre-buffer, 20-600 ms, `GET/POST /latency`, saved in NVS, default 50 since 2026-10-04; it was 170) plus the output stage (I2S DMA 6 x 256 frames = 32 ms + one 5.3 ms mixer block). Measured with the mic (includes its ~45 ms): Low 50 ms -> 206 ms, Normal 170 ms -> 316 ms, Safe 400 ms -> 550 ms; the differences match the buffer sizes. Low ran 40 s with the buffer at 24-41 ms and 0 underruns on wired Ethernet. The sender uses TCP_NODELAY and a 32 KB send buffer; TCP adds about 1 ms. Further savings would be VB-Cable's own Max Latency setting (its control panel), a shorter I2S DMA queue, or both at some risk of glitches. The Brick now syncs its clock with SNTP (pool.ntp.org, TZ fixed to Central European time in net.c); `status.time` / `time_synced`, shown in the Status card. Announcements and ducking (2026-10-04): `POST /announce` (login, body = a .mp3 or 16-bit .wav up to 400 KB) plays a sound from memory (no flash write, so no stream glitch): alone if the main channel is free, otherwise mixed over what plays through the one-shot channel. While a clip or announcement plays over the main channel the main channel is lowered by `duck_db` (`GET/POST /duck`, 0-30, default 12, saved in NVS; mic test: -17 dB during, back to 0.0 dB right after). `status.boot` = {reason, abnormal, count, crashes} from `bootinfo.c` (reset reason of the last start, boots and abnormal restarts counted in NVS), shown as 'last restart' in the Status card; the unexplained reboot seen on 2026-10-04 11:12 is what prompted it. Connectors and free pins: see docs/connectors.md (QWIIC/I2C CN1 on GPIO21/27, 30-pin FPC for an OLED on GPIO15/4/32 plus SPI, free GPIO34/2/13, unfitted I2S mic header). Bluetooth A2DP sink (2026-10-05, `bluetooth.c`, docs/bluetooth.md): works with a phone (5 min, 0 drops, 0 underruns); explicit pairing window (2 min from the page, 3 min after boot by default); needs an external antenna on the ESP32-WROVER-IE module (without it the range is centimeters: -89 dBm vs -46 dBm after attaching one). Windows 11 now works too (2026-10-05): the cause of the earlier failure was Secure Simple Pairing, which makes Bluedroid add MITM protection to the A2DP service, so the Brick re-authenticated every incoming audio connection and a Realtek adapter never answered; the default is now legacy PIN pairing (0000, only in the pairing window), setting `ssp` / `POST /bluetooth?ssp=1`. Windows source measured: 600.0 Hz at the speaker, 0 drops, 181 ms click-to-sound as heard by the mic (~135 ms real, lower than the wired cable route at the Low setting). One Bluetooth connection at a time. Tools: windows_bt_pair.py, windows_bt_connect.py (retries), Capture-WindowsBluetoothTrace.ps1, bt_latency_test.py. The amp volume was raised to the speaker profile's limit (-21 dB) on request. Big task buffers moved to PSRAM (`EXT_RAM_BSS_ATTR`) and Wi-Fi IRAM options turned off to make the Bluetooth controller fit; the image is now about 1.8 MB of the 2 MB OTA slot. Flash the Brick only when nothing is playing (a flash during a stream cuts it for about 10 s).

v1.1.0 adds: idle amp power-down (Hi-Z after 20 s, PWDN off after 10 min, `/power`, `status.amp`, shown on the page's amp chip; write-up in `docs/tas5825m-power-down.md`) and mDNS (`audiobrick.local`). The page shows streaming state in the Network audio card and the Playback line (buffer ms, dropouts).

All of the following were tested on the real board (2026-10-03), mostly with the PC microphone:
- **VBAN receiver** (UDP 6980) and **Scream receiver** (UDP 4010, unicast and multicast 239.255.77.77), `netaudio.c`: off by default, switched on from the page (`/netaudio`), optional allowed-sender address and VBAN stream-name filter. Verified pitch by mic for 48 kHz/16-bit/stereo, 44.1 kHz/24-bit/mono (resampler), Scream 48 and 44.1 kHz, multicast. A stream you start on the board wins; after you press stop the sender is ignored until it goes quiet for 1 s. The receivers must wait for `net_has_ip()` before opening sockets (opening them at boot crashed lwIP and caused a boot loop, see safe mode). `esp_eth_ioctl` takes a *pointer* to a bool (passing the value crashed the board).
- **Web radio** (`radio.c`, `/radio`, `/radio/play?preset=N|url=`, `/radio/stop`): MP3 over http and https (TLS costs about 30 KB of internal heap; `heap.min` about 56 KB while it runs). Presets in NVS (defaults: Radio Paradise, KEXP; SomaFM refused the request with 403). Non-MP3 addresses are refused ("not MP3 (text/html...)"). The player's pause stops a radio stream.
- **OSC synthesizer** (`synth.c`, UDP 9000, `/synth`, `/synth/note`): 8 voices, sine/triangle/saw/square (PolyBLEP), ADSR, master low-pass, soft clip; notes below the speaker profile's lowest frequency are ignored; held notes stop after `max_note_s` (default 30 s). OSC `/synth/note n vel` without a noteoff keeps sounding (my own test once forgot this). Mic test passed: 439.8 Hz, chord, square harmonics, bundle.
- **Safe mode** (`safemode.c`): three boots in a row that did not stay up for 30 s switch off the optional features (receivers, synth, ambient) for that boot; page and OTA still work; `status.safe_mode`. Verified with four forced resets and recovery.
- **Idle**: output fades to exactly 0 when the last source ends; amp mutes after about 1 s of silence, goes Hi-Z after `hiz_after_s` (default 20 s) and powers down via PWDN after `off_after_s` (default 600 s); any sound wakes it (v1.1.0, `GET/POST /power`, `status.amp`; `tools/amp_power_test.py` checked transitions with the mic: no click above a normal tone start, but typing/fans near the mic spoil the 2-20 kHz measurement, so keep the room quiet).
- **Timing** (mic, includes about 40-50 ms mic delay): OSC note to sound about 62 ms (same with the amp idle or awake), test tone 80 ms, starting a clip about 172 ms (170 ms pre-buffer), boot to web page 4.9 s, boot to first sound 5.3 s.
- **Loops**: `tools/check_loop.py` / `tools/make_loop.py`; only clips that pass may be called `*_loop.mp3` (all five do now: rain 17.5 s, PC fan 8 s, fridge 13.8 s, kettle 13.3 s, birds 22.8 s). A linear crossfade of unrelated sounds dips by 3 dB; use equal-power.
- Renamed `limits.h/.c` to `speaker_limits.*` because a header called `limits.h` shadows the system one (broke mbedTLS).
- Home Assistant package updated (radio preset and synth note services); still not tested against a real Home Assistant.
- Windows side (see `docs/windows-setup.md` with checked download links): VB-Audio Virtual Cable is installed on the PC and the chain Windows sound -> cable -> `tools/stream.py` -> Brick was verified with the real cable (699.8 Hz at the speaker). Voicemeeter and Scream are NOT installed. The official Scream driver cannot be installed normally on Windows 11: its certificate expired 2023-07-07, no fork ships a signed build, and the workaround needs Secure Boot off plus Test Mode (which blocks VRChat's Easy Anti-Cheat). Do not enable Test Mode without the user's explicit consent. Voicemeeter's drivers are properly signed and are the clean route to VBAN.
- **Build-time features (v1.0.0)**: `main/Kconfig.projbuild` menu "Audio Brick features" (`idf.py menuconfig`): EQ, ambient scene, VBAN, Scream, radio, synth. A feature that is off is not compiled (no code, no endpoints, no page card; `/status.features` tells the page). **Scream is OFF by default** (the Windows driver cannot be installed normally on Windows 11). In C use `#if CONFIG_AB_FEATURE_X` in the preprocessor and `AB_HAS_X` (features.h) in expressions, because an unset Kconfig bool is undefined, not 0. Presets: `sdkconfig.defaults.full`, `sdkconfig.defaults.minimal`. `sdkconfig.defaults` only applies when `sdkconfig` does not exist. Sizes: default 979 KB, full 992 KB, minimal 693 KB. All three configurations were built and the default one was flashed and tested.
- mDNS (v1.1.0, `AB_FEATURE_MDNS`, default on): `audiobrick.local` plus `_http`, `_osc`, `_vban` services; it keeps W5500 all-multicast on. The first lookup of a .local name from Windows can take 2-3 s; scripts should resolve once.
- Windows route in use now (2026-10-04): **VB-Cable + `tools/brick_sender.py`** as a hidden logon task (`tools/Install-BrickSender.ps1`): Windows has ONE playback device ("Speakers (VB-Audio Virtual Cable)"; the 16-channel twin is hidden by `tools/Set-SingleBrickAudioDevice.ps1`). The sender connects to TCP 4010 only while there is sound and disconnects after 5 s of silence (amp can idle). Tested with the mic: 599.9 Hz. Voicemeeter (VBAN) worked end to end too (route 0, 2871 packets, 0 dropped) but adds about 18 devices and was uninstalled. Lessons: (1) hiding ANY VB-Audio device made Voicemeeter silent (levels 0) until everything was restored and the Windows Audio service restarted; (2) Voicemeeter's first strip is the PC microphone, routed to speakers and VBAN: feedback wobble of +-80 %, mute the hardware strips; (3) `Disable-PnpDevice` does nothing for audio endpoints, use `IPolicyConfig::SetEndpointVisibility` (full endpoint id with the `{0.0.0.00000000}.` / `{0.0.1.00000000}.` prefix; the registry DeviceState read afterwards is stale); (4) VB-Cable ignores Windows' volume/mute (level at CABLE Output identical at 100/50/20 %), so `brick_sender.py` follows the cable device's Windows slider (pycaw) and sets the Brick amp volume via `/volume?level=` (up max 0.05 per 0.25 s, down at once; slider initialised from the Brick; mute silences on the PC); mic test: 50 % = -21 dB, 25 % = -33 dB, mute = digital silence; (5) a firmware update (Brick reboots about 10 s) used to cause huge audio/video lag while YouTube played through the cable: the sender blocked in `sendall` and replayed up to 8 s of queued audio. Fixed: live-audio queue of at most 300 ms (oldest dropped), 1 s send timeout, 32 KB send buffer (Windows would otherwise buffer megabytes), flush after every reconnect; verified with `tools/sender_stall_test.py` (reconnect 2.3 s, audio age 0). Expect a few seconds of silence, not lag, during an OTA; (6) an always-on sender of digital silence would keep the amp awake: the VBAN/Scream receivers now treat 4 s of silence as idle (`netaudio.c`), the TCP sender disconnects itself.
- Not done: AAC radio, a Windows tray app.

- v0.8.x: **three-channel mixer** (`media.c`, `tone.c`): SLOT_MAIN (stream or clip started by the user), SLOT_BG (ambient loop), SLOT_EVENT (ambient one-shots), each with its own PSRAM ring (256/128/128 KB); two channels together are scaled 0.75. Pause fades out the main channel without losing its place. **Player transport** (`player.c`): play/pause/toggle/next/prev/stop over the alphabetical clip list, optional auto-advance; endpoints `/player`, `/player/play|pause|toggle|next|prev|stop|autonext`. **Ambient scene** (`ambient.c`, `ambient_http.c`): one background loop plus random-interval events (rules in NVS, up to 8; default scene is rain + meow/typing/door, OFF); the scene pauses itself while the main channel or the tone plays. Endpoints `/ambient` (GET/POST), `/ambient/enable?on=`. `/volume?level=0..1` maps -70 dB up to the speaker-profile cap; `/status` has `volume_level`, `player`, `ambient`, `heap`.
- Idle behaviour (0.8.5): when the last source ends, the output decays smoothly to exactly 0 (time constant 0.12 s) instead of stepping, and the amp is muted only after about 1 s of digital silence. Measured with the mic: a stopped clip fades to the noise floor in about 0.3 s with no high-frequency spike.
- Limits learned: the web server handler table had to grow (now 48 handlers); lwIP sockets 16; two simultaneous MP3 decoders use roughly 60 KB of internal heap each (28 KB task stack + buffers): `heap.min` was about 90 KB with background + event playing, so do not add more decoders casually.
- Home Assistant: `homeassistant/audiobrick.yaml` (REST sensors, rest_commands, `universal` media player with play/pause/next/previous/volume, ambient switch). Not tested against a real Home Assistant yet. Planned next: web radio (HTTP MP3 streams into the main slot), VBAN receiver (UDP 6980, works with Voicemeeter) and a Scream receiver (virtual Windows sound card, UDP 4010 multicast 239.255.77.77).

- v0.7.4: gapless MP3 (`clip.c` play_mp3): skips the ID3v2 tag, reads the Xing/Info + LAME tag in the first frame, drops that metadata frame and trims encoder delay (+529 decoder delay samples) and padding, so looped clips have no 25-35 ms gap at the join. Without a tag it skips 1105 samples. Verified in the serial log ("mp3 pass: ...") on `cat_meow_1` (576 + 1504 trimmed) and `rain_window_loop` (two identical 24.20 s passes). Not yet confirmed by ear. The EQ applies to clips like every other source (same audio task).
- Planned, not built: a random-timing player (pick clips, min/max interval, volume; for example a meow every 5-20 min, typing bursts, rain loop underneath).

- v0.7.3: lwIP socket pool raised to 16 (`CONFIG_LWIP_MAX_SOCKETS`, web server max_open_sockets 8). With the old 10 the web server stopped accepting connections ("httpd_accept_conn: error in accept (23)") when a browser tab kept connections open next to the stream port; symptoms were curl exit 56 and empty replies, and a delete pass that silently did nothing. If that happens again, close browser tabs and wait about 30 s. Boot volume default -38 dB (cap -31 dB). Index page is sent with `Cache-Control: no-cache`.
- Web page checked in a real browser (Playwright, 2026-10-03): clip table with round play buttons, loop checkbox, EQ card with plot, no console errors. A script edit once deleted the whole EQ block and `node --check` did not notice; check for undefined functions too, and test the page in a browser after JS edits.
- Clips on the board (15, 2.5 MB) are in `clips/` with `ATTRIBUTION.md`: long ones normalised to -18 LUFS, short ones to -3 dBFS peak, five ambient ones (rain, PC fan, fridge, kettle, birds) are seamless crossfaded loops named `*_loop.mp3`. The loop feature (checkbox in the clip table, `loop=1`) repeats until stopped. W5500 note: the ESP-IDF driver runs it in MAC-raw mode with lwIP on the ESP, so its 8 hardware sockets are not used; limits are the SPI link (20 MHz) and the ESP's lwIP.
- Git: the repo is initialised locally (no remote). `.gitignore` excludes build/, sdkconfig, managed_components, recordings (`measurements/*.wav`) and browser scratch. The web password is never stored in the repo.

### Older notes (v0.7.0)

- v0.7.0 adds a software speaker EQ (`eq.c`, `eq_http.c`): up to 6 RBJ biquad bands (peaking, low shelf, high shelf), identical on both channels, applied to every source in the audio task; settings in NVS; automatic preamp cancels the largest boost; endpoints `GET /eq`, `POST /eq` (text body lines `enabled=1`, `b0=on,type,freq,q,gain`), `POST /eq/reset`; page card with live response plot. Starter preset (from the right NS-B40 measurement): peak 250 Hz Q0.8 -3 dB, high shelf 6 kHz Q0.7 +3 dB, so preamp -3 dB. Verified acoustically: change within about 1 dB of design from 400 Hz to 10 kHz; 100-315 Hz measured deeper than designed (up to 4 dB at 160-200 Hz), probably measurement error in the bass.
- Right NS-B40 (cleaner, 3 runs, padded setup): bump +4 dB at 250 Hz, notch about -13 dB near 2.2 kHz (also on the left), roll-off above 6 kHz (-14 dB at 8 kHz, -28 dB at 10-12 kHz). Both speakers are the same model, so one EQ serves both.
- v0.7.1: clip manager on the page (storage bar, drag-and-drop multi-upload with progress, per-clip play/loop/stop/download/rename/delete, playing clip highlighted); firmware adds `loop=1` on `/clips/play`, `GET /clips/download` (login), `POST /clips/rename?name=&to=`. Tested: upload, rename, byte-identical download, loop past the file length, delete refused while playing.
- Clips for a lifelike feel are collected in `clips/` (with `ATTRIBUTION.md`); ideas: cat, big cat, PC fan, typing, rain, clock. Not yet implemented: looping and random-interval triggers.

### Earlier notes (v0.6.0)

- v0.6.0 adds a shared player (`media.c`: 256 KB PSRAM ring, 48 kHz stereo s16, linear resampler for other rates), a TCP stream port 4010 (`stream.c`; protocol in `stream.h`; sender script `tools/stream.py`, needs ffmpeg), stored clips on the FAT `storage` partition (`clip.c`, `storage.c`; MP3 via minimp3 or 16-bit PCM WAV; `clips_http.c` endpoints `/clips`, `/clips/upload|play|delete`, `/media/stop`, `/volume`), a dead-man timer on the test tone (15 s unless refreshed), safe sliders and overload indicators on the web page. Stream tested OK (3 s test tone, no underruns). Default amp volume is now -45 dB.
- Gotchas hit: (1) `sdkconfig.defaults` is ignored once `sdkconfig` exists; delete `sdkconfig` to apply new defaults. (2) The MP3 decoder needs a 32 KB task stack (12 KB overflowed and rebooted the board). (3) Serial flashing the new partition table once was required; since then all updates go over Ethernet. Clip test (v0.6.1): 44.1 kHz mono MP3 and 22.05 kHz mono WAV both play, no underruns; uploading or deleting clips while playing may glitch (flash writes stall the cache).
- Speaker measurement: `tools/measure.py HOST --mic "Sennheiser Profile" [--play]` plays one quiet log sweep (80 Hz-20 kHz, 6 s, about 55 dB SPL at 1 m, left channel, refuses to exceed the profile limit) over the stream port and records it with the PC microphone; `--analyse WAV` re-processes a saved recording. Output in `measurements/` (wav, csv, png; 1/6 octave, gated 8 ms plus 60 ms). The mic's own on-axis response (`tools/mic_sennheiser_profile_0deg.csv`, digitised from the Sennheiser spec sheet; flat within about 3 dB above 125 Hz, rolls off below 100 Hz: -3 dB at 100 Hz, -17 dB at 50 Hz) is subtracted by default. First NS-B40 result (mic placement not verified): +4 dB hump near 250 Hz, dip near 3 kHz, ragged peaks 3.6/5.2/6.8 kHz (looks like cone breakup of a single full-range driver), then a steep roll-off above about 9 kHz. Do not boost narrow dips; plan is cut-only or gentle broad correction as software biquads in the audio task (not yet implemented).
- Bug fixed in v0.6.2: the end of every stream/clip played a 100 ms full-scale 440 Hz beep (the idle tone generator inherited the media gain of 1.0). It also polluted sweep recordings made before the fix; ignore `measurements/` files from before 2026-10-03 18:40 except as rough context. Known remaining blemish: a small thump (about -62 dBFS near 220 Hz at 1 m) at the start and end of playback from the amp mute/unmute. Aborting a stream mid-signal cuts abruptly (no fade).
- Measurement caveats: the first NS-B40 sweeps were on a bare desk (strong desk bounce, notch near 3 kHz); the padded setup after the fix repeats within about 1 dB (0.3-1 kHz and above), but still shows a steep treble loss (about -30 dB at 8 kHz) on both speakers. Not yet known whether that is the speaker, the aim of the mic, or the Brick's chain; an A/B with a different source through the same mic would tell.
- The safe-slider rule (also used for the playback volume): each step up limited to 2 dB, Page/Home/End disabled.
- Hearing/EQ idea queued by the user: calibrate speaker EQ with a Sennheiser Profile USB-C mic (consumer mic, no calibration file, so only relative corrections) or look up EQ for the NS-B40 online. The TAS5825M has a 15-band EQ (biquad coefficients in book 0xAA; not yet implemented).

### Older notes (v0.4.0)

- v0.4.0 adds: web page at `http://<ip>/` (public status, estimated loudness; cookie login unlocks tone test, speaker profile, firmware upload, password change), speaker profile + SPL limiter (`limits.c`), PVDD and fault registers in `/status`, `/tone` `vol=` parameter. Auth: POST `/login` (body = password) sets an HttpOnly 30-day `sid` cookie; scripts can still send the password as `X-Token`. Initial password = the 32-hex token printed on the serial log; changeable on the page. 5 failed logins lock out for 30 s. Plain HTTP, LAN only.
- Loudness limiter: SPL = sens + 20 log(V/2.83) - 20 log(distance). Default profile: Yamaha NS-B40, 6 ohm, 83 dB, 1 m, **70 dB max**, 30 W, lowest tone 100 Hz. Cap enforced in `dac_set_volume_db` (volume register) and `tone_set` (min frequency); absolute ceiling 100 dB. FULLSCALE_VPK = 29.5 V (datasheet value, estimate; not calibrated).
- USB-only measurements: PVDD about 4.39 V (the chip's PVDD ADC, reg 0x5E).
- Partition table now includes `storage` (FAT, ~11.9 MB at 0x420000) but it is NOT yet flashed to the board; the board still has the older table without it. Flashing it needs the serial BOOT/EN procedure once. Planned: compressed clips (Opus or MP3) in storage, plus a raw PCM TCP stream (48 kHz, 16-bit stereo) for PC audio.

### Earlier notes (v0.2.0 API, now superseded by the above where they differ)

- Modules in `main/`: `net.c` (W5500 + DHCP, hostname audiobrick), `dac.c` (TAS5825M, init sequence from mrtoy-me/esphome-tas58xx, starts muted at -30 dB), `tone.c` (I2S 48 kHz 16-bit, sine task with 100 ms ramps; level cap -6 dBFS), `ota_http.c` (HTTP API), `main.c` (LED: blue = no IP, green = online, red = amp fault).
- Partitions: nvs, otadata, ota_0 and ota_1 (2 MB each), rollback enabled. The image is marked valid after about 5 s online.
- Board got 192.168.2.40 via DHCP (may change; check your router or the serial log).
- API (token = 32 hex chars, generated on first boot, stored in NVS, printed on the serial log at every boot; send as `X-Token` header):
  - `GET /status` (no auth): version, partition, uptime, fault/warning pins, tone state.
  - `POST /tone?on=1&freq=440&db=-30` (auth): sine test tone. Refused while FAULTZ is active.
  - `POST /update` (auth): body = `build/audiobrick.bin`, then it reboots.
- OTA workflow: `idf.py build`, then `curl -X POST -H "X-Token: $T" --data-binary @build/audiobrick.bin http://<ip>/update`.
- Serial recovery (partition table changes only): BOOT/EN download mode, then the esptool recipe above. Boot log: reset over RTS while BOOT is released.
- Sine tone verified with speakers by the user on 2026-10-03 while powered from USB-C only (no DC supply). So the amp stage does run from USB 5 V at low power. Start at low volume. Tone starts disabled at every boot.

Test speakers: two Yamaha NS-B40 satellites (6 ohm, 30 W nominal / 100 W max, 83 dB sensitivity, small drivers). Fit a 12 V supply; avoid long high-level sine tones at high frequencies (tweeter heating).

## Planned experiments

1. **Smoke test:** flash stock Squeezelite or Snapclient, play audio into a dummy load or speaker at 12 V, then confirm Ethernet and LED.
2. **Synthesizer:** I2S synth on the ESP32 (wavetable or FM, 44.1 or 48 kHz stereo), controlled over MIDI-over-network or OSC.
3. **OSC control:** UDP OSC server over W5500 Ethernet (e.g. `/synth/freq`, `/synth/vol`, `/mute`), mapping onto I2S sample generation and TAS5825M volume registers.
4. **Bluetooth A2DP sink (optional):** ESP32 only. Ethernet remains the preferred path. Check Wi-Fi/BT coexistence if both are on.
5. **OSC-controlled audio e-stim pattern generator:** see the safety section. Do not connect anything to the body until the safety checks are done.

## Planned: two Bricks in sync (noted 2026-10-04, not implemented)

The owner expects a second Audio Brick (probably the ESP32-S3 replacement; pins differ, see the pin table) and wants both to play in sync, as a stereo pair
or multi-room. Needs timestamped audio plus a clock sync between the sender and each Brick. Internet NTP (about 5-30 ms) is too coarse; on a wired LAN a
four-timestamp exchange (like NTP) gets to about 1 ms. Playback then happens at `pts + fixed delay`, and the Brick trims the small drift between the
sender's clock and its own I2S clock by dropping or repeating a sample now and then (or resampling by +-100 ppm).

Why not the existing network audio inputs: **VBAN** packets carry only a running packet counter (loss/reorder detection), no timestamp, and Scream has none
either, so two receivers cannot be lined up from them; with a plain TCP/UDP stream each Brick just starts after its own pre-buffer. Snapcast's WireChunk and
RTP carry timestamps (RTP plus RTCP sender reports, or AES67 with PTP, are the standard way, heavier than needed here).

Routes weighed:
1. **Snapcast-compatible client** in the firmware (open protocol, TCP 1704: Hello, ServerSettings, CodecHeader, WireChunk, Time messages; PCM codec is enough).
   Pros: proven sync, per-client volume/mute, groups, ready apps and Home Assistant integration. Cons: needs a snapserver (Windows build exists) on the PC or a Pi,
   fed from the VB-Cable; the sender's silence/idle handling and the volume-key follower would have to be reworked around it.
2. **Own timestamped stream** (UDP, multicast so N Bricks get one copy) with a small LAN time-sync service, built into `tools/brick_sender.py`.
   Pros: no extra server, fits the current one-device setup. Cons: own protocol to design, test and maintain, nothing else can talk to it.
3. Both, own protocol first.

Already in place that helps: SNTP clock on the Brick (`status.time`), adjustable pre-buffer (`/latency`, a stream's delay is about the pre-buffer plus the PC chain),
the hidden sender with silence handling and stall recovery. Testable on ONE Brick before the second arrives: play a click on the PC's own speakers at time T and
have the Brick play the same click at T + D, and compare the two with the microphone. A pair also needs a per-Brick channel select (left / right / both).

## E-stim safety (read before experiment 5)

This board is a 5-26 V, up to 65 W amplifier. It is not a body-safe output.

- Never connect electrodes directly to the speaker terminals.
- Use a purpose-built isolation stage (audio transformer into current-limited output with series resistance), or a commercial audio-input e-stim unit that does its own isolation and limiting. Establish isolation and current limit first.
- Develop and test on the lowest supply voltage (5 V) and a dummy load or scope. Raise the supply only after the output stage is characterized.
- Power the whole rig from battery or an isolated supply. Do not leave a mains-connected USB host or other earth-referenced equipment attached while electrodes are on the body.
- Never place electrodes where current can cross the chest or heart, or on the neck or head. Do not use with a pacemaker or implant.
- Firmware must include:
  - A hardware-backed volume cap.
  - A hard mute on OSC timeout or network loss.
  - A hard mute on FAULTZ.
  - Soft-start ramps only (no step changes in amplitude).
  - No DC offset on the output, since DC in tissue causes burns.
- Keep a physical kill (power disconnect) within reach.

## Open questions

- Power connector type and polarity on the owner's actual revision (4-pin per the shop pages vs barrel jack plus 2-pin on rev C1).
- Whether USB 5 V and the DC input can be connected at the same time.
- Which firmware to test first.
