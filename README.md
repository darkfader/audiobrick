# Audio Brick firmware (ESP32 + TAS5825M + W5500), built with ESP-IDF

Custom firmware for the **Sonocotta Esparagus Audio Brick, ESP32 variant** (not the S3): an ESP32 with a TI **TAS5825M** class-D
amplifier, a **W5500** Ethernet chip and a WS2812 status LED. No Arduino, no PlatformIO, no ESPHome: plain ESP-IDF 5.5, a small web page
for everything, and updates over Ethernet. Written and tested on one real board with two Yamaha NS-B40 speakers.

Board and hardware: <https://github.com/sonocotta/esparagus-media-center> (hardware files and the stock firmware).
This repository is an independent community project, not made or endorsed by Sonocotta.

## What it does

- **Web page** (`http://audiobrick.local/`): status, amp faults and supply voltage, login with a cookie, firmware update, everything below.
- **Safe loudness:** a speaker profile (impedance, sensitivity, distance, maximum dB SPL) turns into a hard volume cap in the amp; all sliders
  only step up in small increments; a test tone stops by itself. The SPL figures are estimates, not measurements.
- **Sound sources:** a test tone, clips stored on the board (MP3/WAV, file manager on the page, mixed over a live stream), internet radio (MP3, HTTP and
  HTTPS), PC sound over TCP, VBAN (Voicemeeter), Scream, and an **OSC-controlled 8-voice synthesizer** (UDP 9000).
- **EQ** from your own measurements (up to 6 biquad bands with automatic preamp; `tools/measure.py` measures a speaker with a USB microphone).
- **Ambient scene** (rain plus random cat / door / typing sounds), **player** controls, **Home Assistant** package (`homeassistant/audiobrick.yaml`, not tested
  against a real Home Assistant yet).
- **Idle power saving:** output fades to zero, the amp mutes, goes Hi-Z after 20 s and powers down after 10 minutes, and wakes with no audible cost
  (measured, see [docs/tas5825m-power-down.md](docs/tas5825m-power-down.md)).
- **Bluetooth audio** (ESP32 only): a phone can play music on the Brick through an explicit pairing window; see [docs/bluetooth.md](docs/bluetooth.md) (tested with one phone and with a Windows 11 PC, one device at a time; **the module needs an external antenna**).
- **Announcements:** `POST /announce` plays an uploaded MP3/WAV (for example a Home Assistant text-to-speech message) over whatever is playing; the stream is lowered meanwhile (adjustable ducking). The page shows why the board last restarted.
- **Sleep timer, quiet hours and an alarm** (a stored clip at a set time and weekdays): one page card, see [docs/architecture.md](docs/architecture.md) for the switch. The amp volume is remembered across restarts.
- **Adjustable streaming delay** (Low about 50 ms, the default; Normal about 170 ms; Safe about 400 ms buffer) and a network-synced clock (SNTP).
- mDNS (`audiobrick.local`), safe mode after repeated crashes, rollback-protected OTA, a **Wi-Fi client** with remembered networks as a second interface (off by default; **while it is on, clips played over a Bluetooth stream can make the music stutter**, see [docs/wifi.md](docs/wifi.md)). **Every optional component is a build-time switch** (`idf.py menuconfig`, "Audio Brick features"; the all-off build is 655 KB), see [docs/architecture.md](docs/architecture.md).

## Getting started

1. Install ESP-IDF 5.5 (<https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/get-started/index.html>).
2. `idf.py build`. The partition table has two 3 MB firmware slots and a 10 MB storage area (changing it later needs a serial flash again, and erases the stored clips). The first flash is over USB-C (CH340): on this board the automatic reset into download mode did not work, so hold BOOT, tap EN,
   release BOOT, then `python -m esptool --chip esp32 -p COMx -b 460800 --before no_reset --after hard_reset write_flash "@flash_args"` from `build/`, and tap EN once to start it (the automatic reset after flashing does not work either).
3. Plug in Ethernet. The serial log prints the web **password** (32 hex characters, generated on first boot, changeable on the page).
4. Later updates: `curl -X POST -H "X-Token: <password>" --data-binary @build/audiobrick.bin http://audiobrick.local/update`, or use the page.

## Sending your PC's sound to it (Windows)

Recommended: **one** playback device, no extra windows: VB-Cable plus a hidden background sender. See [docs/windows-setup.md](docs/windows-setup.md)
(also covers Voicemeeter/VBAN, and why Scream is not recommended on Windows 11). The sender follows the Windows volume keys.

## Documentation

- [docs/architecture.md](docs/architecture.md): **start here to read the code**: folders, audio path, threads, settings, how to add a source, the component switches.
- [docs/wifi.md](docs/wifi.md): the Wi-Fi client, what it costs in memory, and the known limit with Bluetooth plus clips.
- `installer/` and `.github/workflows/release.yml`: a browser installer and release builds. Pushing a tag `vX.Y.Z` builds the firmware in CI, attaches `audiobrick-factory.bin` (first install, flash at offset 0) and `audiobrick-ota.bin` (network update) to a release, and publishes a browser installer on GitHub Pages (needs Pages set to "GitHub Actions" once). Not run yet.
- [CLAUDE.md](CLAUDE.md): detailed project notes, pinout, measurements, lessons learned (written while building this; long).
- [docs/bluetooth.md](docs/bluetooth.md): Bluetooth audio, pairing rules, antenna, test results and known problems.
- [docs/connectors.md](docs/connectors.md): the board's connectors, free GPIOs and what they could be used for.
- [docs/windows-setup.md](docs/windows-setup.md), [docs/tas5825m-power-down.md](docs/tas5825m-power-down.md), [docs/tas5825m-features.md](docs/tas5825m-features.md)
- [tools/README.md](tools/README.md): measurement, test and Windows helper scripts.

## Safety

The Audio Brick is a 5–26 V, up to 65 W amplifier. Keep volumes sensible: the loudness figures are estimates, speakers are not omnidirectional and rooms add
level. **Do not connect electrodes or anything that touches a body to the speaker terminals**: the output is not isolated or current-limited (see the
e-stim section in CLAUDE.md). Power the board from a current-limited supply and check the connector polarity of your board revision before powering it.

## Credits

Hardware by Sonocotta. The TAS5825M power-up register sequence follows the one in [mrtoy-me/esphome-tas58xx](https://github.com/mrtoy-me/esphome-tas58xx)
(via rmalchow/ondaire); the registers marked "vendor" in `main/audio/dac.c` are copied as-is. MP3 decoding: [minimp3](https://github.com/lieff/minimp3) (CC0).
Sound clips from Wikimedia Commons under their own licenses: [clips/ATTRIBUTION.md](clips/ATTRIBUTION.md).

## License

MIT for the code, tools and documentation in this repository, see [LICENSE](LICENSE). The clips, minimp3 and build-time components keep their own licenses.
