# Bluetooth audio (A2DP sink) on the Audio Brick (ESP32 only)

A phone or PC can play music on the Brick over classic Bluetooth. This works on the **ESP32** board only: the ESP32-S3 has no classic Bluetooth (only BLE),
so the S3 version of the Audio Brick cannot do this. Code: `main/sources/bluetooth.c`, page card "Bluetooth audio". Status of what has been tested: see the end.

## Check the antenna first

The upstream rev C1 BOM (part U5) fits an **ESP32-WROVER-IE-N16R8** module (16 MB flash, 8 MB PSRAM, matching what esptool reports on the test board): the "IE" variant has **no printed antenna**, only a tiny u.FL socket for an external one.
On the board this was developed on nothing was plugged into that socket, and Bluetooth range was a few centimeters: a scan from the Brick heard one device at
**-89 dBm**; after a small 2.4 GHz antenna was attached the same device was **-46 dBm** (43 dB stronger) and the Brick heard more devices.
If Bluetooth (or later Wi-Fi) is weak, look at the module for the little round socket and attach an antenna.
`POST /bluetooth/scan` (login) makes the Brick scan for 10 s and `GET /bluetooth/scan` lists what it heard with signal strength: a quick range check.

## How it works

- The stack (Bluedroid, classic only, one connection) is only started when Bluetooth is switched on on the page (setting saved in flash). It costs roughly 27 KB of internal
  RAM; most of its memory comes from PSRAM. The decoded audio (SBC, 44.1 or 48 kHz stereo) goes into the same player channel as the network inputs (resampled to 48 kHz),
  so the volume cap, EQ, ducking, power saving and the "first come, first served" rule apply. If something else already plays, Bluetooth audio is dropped.
- **Pairing method: the PIN 0000 by default.** The Brick pairs with the old PIN method (fixed PIN `0000`, only accepted while the pairing window is open), not with
  "Secure Simple Pairing" / "just works". Reason (found with a Windows trace and the stack's debug log): with Secure Simple Pairing on, Bluedroid adds "man-in-the-middle protection" to
  the security requirement of every service, a screen-less speaker can only make an unauthenticated "just works" key, so for each incoming audio connection the Brick starts a second
  authentication round to try to upgrade the key. A Windows PC with a Realtek-based adapter never answers it, the Brick's controller then stops sending data, and the audio link never opens
  ("That didn't work"). Phones tolerate it. With the PIN method that requirement does not exist and both the PC and the phone work. Switch back with `POST /bluetooth?ssp=1` or the page
  checkbox (restarts the Brick; devices have to be paired again). If a phone refuses to pair after switching, remove the old "Audio Brick" entry on the phone first (a stale pairing from the other method makes it say "failed").
- **One device at a time.** The Bluetooth controller is limited to one connection (saves RAM). While your phone is connected the PC cannot pair or connect, and the other way round; to switch,
  disconnect on the device that has it, or switch Bluetooth off and on on the page. A paired device that is in range may reconnect by itself.
- **Pairing is explicit.** The Brick is only connectable by devices it has bonded with. A new device can pair while the *pairing window* is open: it opens when you press
  "Pair a new device" (2 minutes), and, as a setting (on by default), for 3 minutes after every start. Pairing requests outside the window are refused. The window
  closes after one device has paired. "Forget all paired devices" removes the bonds. "Off" means invisible and refusing connections (the stack itself is only unloaded by a restart).
- After pairing, and for about two minutes after a start, the Brick tries to connect back to the device it was last connected to (the way headphones do).
- AVRCP (remote control) is registered because Windows and phones expect it next to A2DP; the playback controls and absolute volume are not used yet.
- Page and API: `GET/POST /bluetooth`, `POST /bluetooth/pair`, `/bluetooth/connect`, `/bluetooth/forget`, `/bluetooth/scan`. `GET /bluetooth` needs the login (it shows device names).

## Tested (2026-10-05, one board with an external antenna, wired Ethernet)

| What | Result |
|---|---|
| Phone pairs through the pairing window and connects | works, with the PIN method and with "just works" (after removing an old pairing of the other kind from the phone) |
| Sustained playback, 5 minutes | 42 packets/s, 0 dropped, 0 dropouts, buffer 62-77 ms, lowest internal RAM 80 KB, no growth |
| Pairing a Windows 11 PC (TP-Link UB500 / Realtek dongle) with `tools/windows_bt_pair.py` | works (Windows confirms pairing itself) |
| Windows connecting the audio link | **works with the PIN pairing** (not with "just works", see above). `tools/windows_bt_pair.py` pairs (answering the PIN), `tools/windows_bt_connect.py` asks Windows to connect (it retries for up to 40 s while Windows installs the audio service); Windows then lists a playback device "Headphones (Audio Brick( Stereo ))" at 44.1 kHz. Test: 600 Hz tone from Windows to that device came out of the speaker at 600.0 Hz, 166 packets, 0 dropped, 0 dropouts, buffer 61 ms |
| Latency over Bluetooth, Windows source | **181 ms** as heard by the microphone (median of 24 bursts, 168-210 ms; includes the mic's own ~45 ms, so about 135 ms real), measured with `tools/bt_latency_test.py`. That is lower than the wired VB-Cable route at the Low setting (206 ms heard). A phone adds its own encoder and buffer delay and has not been measured. The Brick reports its delay to the source over AVDTP (pre-buffer + 40 ms) |
| Sound quality, other phones, Wi-Fi at the same time | not measured yet |

## Memory and flash notes

Enabling Bluetooth (the controller needs a lot of fixed RAM) pushed the build over the internal RAM limit, so the big task buffers (`stream.c`, the HTTP scratch buffers,
the Bluetooth resampling buffer) moved to PSRAM (`EXT_RAM_BSS_ATTR`, needs `CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY`) and the unused Wi-Fi fast paths were moved out of IRAM
(`CONFIG_ESP_WIFI_IRAM_OPT=n`, `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`, `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y`). Free internal RAM with nothing playing went from about 166 KB to about 118 KB
(Bluetooth code and Wi-Fi libraries are linked in), and about 91 KB with the Bluetooth stack running. The firmware image grew from about 1.05 MB to about 1.8 MB in a 2 MB OTA slot,
which leaves little room for the planned Wi-Fi client. Switch the feature off with `CONFIG_AB_FEATURE_BLUETOOTH` (menuconfig, "Audio Brick features") if you do not need it.
Two MP3 decoders at once (a clip over a stream) use about 60 KB of internal RAM each, so with Bluetooth running keep an eye on `heap.min` in `/status`.

## Tools

- `tools/windows_bt_pair.py`: scans for the Brick, pairs this Windows PC with it (opens the pairing window, answers the confirmation) or removes the pairing (`--unpair`). Uses the `winrt` Python packages (Python 3.12 via `uv run`, see the docstring).
- `tools/windows_bt_connect.py`: asks Windows to connect or disconnect the paired Brick's audio (`BluetoothSetServiceState`).
- `sdkconfig.defaults.btdebug`: an overlay that switches on the Bluetooth stack's own detailed logs (it also sets the global log level to debug, which makes the image nearly fill the 2 MB slot and floods the serial port; never leave that build on the board); build it in a separate folder:
  `idf.py -B build_btdebug -D SDKCONFIG=sdkconfig_btdebug -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.btdebug" build`. Read the log from the serial port without resetting the board (pyserial with `dtr=False`, `rts=False`).

## Volume keys (AVRCP absolute volume)

Setting `absvol` (page: Bluetooth card, "Let the phone's or PC's volume control set the amp volume"; `POST /bluetooth?absvol=0|1`, restarts the Brick; off by default). When on, the Brick advertises
absolute volume: the volume control of the phone or PC moves the **amp** volume instead of scaling the audio in the source.

- The remote's 0..127 maps onto the same scale as the page's volume slider: -70 dB up to the speaker profile's cap, so the loudness limit always holds.
- Rising is limited to 2 dB per 250 ms, falling is immediate (like every other volume control here).
- A volume changed on the Brick (page, sender, quiet hours) is reported to the remote ("changed" notification), so the phone's slider follows. Each report makes the phone re-register, which shows in the serial log as `remote registered for notification 13`.
- Tested 2026-10-06 with an Android phone: its volume keys arrived as `remote volume 89/127 -> -35 dB` ... and moved the amp; the slider followed volume changes made on the page.
- **Lesson:** the AVRCP services (`esp_avrc_ct_init`, `esp_avrc_tg_init`) must be initialised **before** `esp_a2d_sink_init()`. In the other order the stack logs `BT_BTC: AVRC not Init, not using it` and no remote-control link ever opens (no volume, no media keys). The Windows connection kept working in the wrong order because the service records exist anyway.
- **Windows 11 does not use it** (tested 2026-10-06): after the init-order fix the PC still connects and plays (a test tone arrived, 0 dropped), and it opens the remote-control link, but it never registers for volume changes, so its volume stays a software volume applied before the audio is sent (as before). Windows' `DisableAbsoluteVolume` is 0, the Brick publishes the amplifier category (CAT2) in its target record and answers the supported-events query, so the reason is not known; a Windows Bluetooth trace (`tools/Capture-WindowsBluetoothTrace.ps1`) of the AVRCP exchange would be the next step.

## Several paired devices

The Brick can be paired with several devices at once (the list is shown as `bonded`; the phone and a Windows PC were both in it). Only **one** device can be connected at a time (`CONFIG_BTDM_CTRL_BR_EDR_MAX_ACL_CONN=1`, and the Bluedroid A2DP sink handles one link). A connected but silent device keeps the link, so another paired device cannot connect until the first is disconnected (Disconnect button, or opening a pairing window). Idea, not built: when a second paired device connects, drop the first one automatically ("last device wins"); it is not known whether the stack accepts the second connection attempt before the first is dropped.
