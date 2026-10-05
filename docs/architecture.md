# How the firmware is built (a guided tour)

This firmware turns an ESP32 with a TAS5825M class-D amplifier and a W5500 Ethernet chip into a networked speaker. It is plain C on ESP-IDF 5.5, about 6,000
lines apart from the HTML page and the MP3 decoder. This page is the map: read it first, then the files in the order given at the end.

## The five folders (`main/`)

| Folder | What lives there | Think of it as |
|---|---|---|
| `app/` | `main.c` (startup order, the status LED loop), `board.h` (pins), `features.h` (build-time switches), `bootinfo.*` (why did it restart?), `safemode.*` (crash-loop protection) | the shell around everything |
| `audio/` | `media.*` (the buffers sources write into), `tone.*` (the mixer and the I2S task), `dac.*` (the amplifier chip), `speaker_limits.*` (the loudness limit), `eq.*` (the equaliser) | the audio engine |
| `sources/` | everything that makes sound: `clip.*` (MP3/WAV from flash), `player.*`, `ambient.*`, `radio.*`, `synth.*` (OSC synthesizer), `stream.*` (TCP), `netaudio.*` (VBAN, Scream), `bluetooth.*`, plus `storage.*` and the `minimp3.h` decoder | the inputs |
| `net/` | `net.*`: the Ethernet interface, DHCP, mDNS, SNTP clock; `wifi.*`: the Wi-Fi client with remembered networks (second interface) | the network |
| `web/` | `ota_http.*` (HTTP server, login, status, firmware update), `playback_http.c` (volume, delay, stop, announcements), `clips_http.c`, `ambient_http.c`, `eq_http.c` (their endpoints), `index.html` (the whole page, embedded in the firmware) | the control surface |

Headers are included by plain name (`#include "media.h"`): `main/CMakeLists.txt` lists all five folders as include directories, so moving a file between folders only means
editing that list.

## The audio path

```
 sources                       audio engine                              hardware
 -------                       ------------                              --------
 clip.c    (MP3/WAV decode) --\
 radio.c   (HTTP MP3)        ---> media.c: three ring buffers ("slots")
 stream.c  (TCP)             --/   MAIN   256 KB  one stream or clip at a time
 netaudio.c(VBAN/Scream)     --/   BG     128 KB  ambient loop
 bluetooth.c (A2DP)          --/   EVENT  128 KB  one-shots (clips over a stream, announcements)
                                          |
 synth.c (OSC, rendered live) ------------+--> tone.c: audio_task (core 1, 5.3 ms blocks of 256 frames)
 test tone (web page) --------------------+      per-slot gain ramps, ducking, mix, EQ (eq.c), decay to silence
                                                 |
                                                 v   I2S, 48 kHz, 16-bit stereo, 6 x 256-frame DMA
                                         dac.c: TAS5825M (volume, mute, Hi-Z, power-down)  ->  speakers
```

- Everything is **48 kHz 16-bit stereo** at the engine. A source with another rate (Bluetooth 44.1 kHz, a radio station, a phone call-quality stream) goes through the small linear
  resampler in `media.c` before it is written.
- A source asks `media_begin(kind, label)` for the MAIN slot, writes frames with `media_write_nb_slot()`, and calls `media_finish()` when it is done. If the slot is busy the
  answer is no: sources do not interrupt each other, first come first served (the web page's Stop button aborts).
- `media.c` holds back playback until the **pre-buffer** (default 50 ms, setting `/latency`) has been collected; for a live stream the buffer then stays at about that level, so it
  *is* the stream's delay. Underruns are counted once per dropout and shown on the page.
- `tone.c` fades the output smoothly to exactly 0 when the last source ends, mutes the amp after about 1 s of silence, and `dac.c` puts it in Hi-Z (20 s) and powers it down (10 min)
  later; any new sound wakes it again with no audible delay.
- The loudness limit is not a software gain: `speaker_limits.c` turns the speaker profile (impedance, sensitivity, distance, maximum dB SPL) into a **maximum amp volume**, and
  `dac_set_volume_db()` clamps to it. The web page, OSC, Home Assistant and Bluetooth all end up there, so none of them can exceed it.

## Threads (FreeRTOS tasks)

| Task | Where | Priority / core | Does |
|---|---|---|---|
| `audio` | `audio/tone.c` | 6, core 1 | the mixer; the only writer of the I2S DMA; never blocks except on the DMA |
| `clip`, `radio` | `sources/` | 4, core 1 | one decoder each (stack 28 KB: minimp3 uses ~18 KB of stack). Stack and work buffers are in PSRAM, so a clip is loaded into PSRAM first (a PSRAM-stack task must never read flash); see docs/wifi.md |
| `stream`, `vban`, `scream`, `osc` | `sources/` | 4 | socket receivers |
| `player`, `ambient` | `sources/` | 3 | the transport buttons and the random-event scheduler |
| HTTP server | `web/ota_http.c` | IDF default | all web handlers run here, one request at a time per connection |
| Bluetooth controller and host | IDF | high, core 0 | not ours; our callbacks in `sources/bluetooth.c` must stay short |

Internal RAM is the scarce resource (about 85 KB free with Bluetooth running): big buffers use `EXT_RAM_BSS_ATTR` or `heap_caps_malloc(MALLOC_CAP_SPIRAM)`.

## Settings, state and security

- **Settings** live in flash (NVS namespace `audiobrick`): `token` (web password), `sessions`, `profile` (speaker), `eq`, `synth`, `netaudio`, `radio`, `ambient`, `power`,
  `prebuf_ms`, `duck_db`, `bt_*` (on, name, pairing method, last peer, open window after start), `boots`/`crashes`/`bootcnt` (diagnostics, crash-loop guard), `vol_db` (amp volume, saved 3 s after the last change), `wifi_on`/`wifi_nets` (Wi-Fi switch and remembered networks; passwords are never sent back by any endpoint).
- **State** (what is playing, buffer fill, faults) is read-only JSON from `GET /status` (public: the logged-out page and Home Assistant use it).
- **Control** is `POST` endpoints. Every endpoint that changes anything checks `web_authorized(req)` first (cookie from `POST /login`, or the `X-Token` header for scripts);
  a few `GET`s that show settings are public on purpose. `tools/` and `homeassistant/` are the clients.
- **Features** can be switched off at build time (`idf.py menuconfig` -> Audio Brick features); code and endpoints of a disabled feature are not compiled.

## Switching components on and off (`idf.py menuconfig` -> Audio Brick features)

Everything optional is a Kconfig switch, so a build contains only what you want. What is always built is the core: the audio engine (`audio/`), the test tone, the speaker-profile
loudness limit, the web page with login, the firmware update, Ethernet and `/status`. A switch that is off removes the code, the endpoints and the page card (the page learns what exists
from `/status.features`).

| Switch | What it adds | Needs |
|---|---|---|
| `AB_FEATURE_CLIPS` | stored clips on the flash file system, clip manager, player buttons | |
| `AB_FEATURE_ANNOUNCE` | `POST /announce` and ducking | clips (shares the decoder) |
| `AB_FEATURE_AMBIENT` | rain plus random events | clips |
| `AB_FEATURE_EQ` | the parametric speaker EQ | |
| `AB_FEATURE_RADIO` | internet radio (MP3 over HTTP/HTTPS, pulls in TLS) | |
| `AB_FEATURE_SYNTH` | the OSC synthesizer | |
| `AB_FEATURE_TCPSTREAM` | TCP stream receiver (port 4010) | |
| `AB_FEATURE_VBAN` / `AB_FEATURE_SCREAM` | the two UDP network audio receivers (Scream is off by default) | |
| `AB_FEATURE_BLUETOOTH` | A2DP sink (ESP32 only) | |
| `AB_FEATURE_MDNS` | the `audiobrick.local` name | |
| `AB_FEATURE_SCHEDULE` | sleep timer, quiet-hours volume ceiling, alarm clip (`sources/schedule.*`) | clock (SNTP) for quiet hours and alarm; clips for the alarm |
| `AB_FEATURE_WIFI` | Wi-Fi client: scan, remember up to 8 networks, join the strongest; second interface next to Ethernet | |
| `AB_FEATURE_SNTP` | network clock | |
| `AB_FEATURE_POWERSAVE` | amp Hi-Z / power-down timers and `/power` | |
| `AB_FEATURE_SAFEMODE` | crash-loop protection | |
| `AB_FEATURE_BOOTINFO` | reset reason and boot counters | |

In C use `#if CONFIG_AB_FEATURE_X` in the preprocessor and `AB_HAS_X` (`app/features.h`) in ordinary expressions, because an unset Kconfig option is undefined, not 0. Headers of optional
components provide inert stand-ins when the feature is off, so callers (for example the status page asking the player what is playing) do not need their own `#if`.

Presets: `sdkconfig.defaults.minimal` (everything optional off: 655 KB), `sdkconfig.defaults.full` (everything on, including Scream) and the default (everything on except Scream: 1.8 MB).
All of them, plus "everything except clips", were built on 2026-10-05; build one with `idf.py -B build_min -D SDKCONFIG=build_min/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.minimal" build`.

## How to add a new sound source (the usual first contribution)

1. Make `sources/mything.c/.h` with an `mything_init()` and, if it has settings or endpoints, `mything_http_register(httpd_handle_t)`.
2. In its task or callback: `if (media_begin(MEDIA_STREAM, "label")) { ... media_write_nb_slot(SLOT_MAIN, frames, n); ... media_finish(); }`. Use `SLOT_EVENT` to mix over what plays.
3. Add the file to `main/CMakeLists.txt`, call `mything_init()` from `app/main.c`, and register the endpoints from `web/ambient_http.c` (`ambient_http_register` is where the feature
   modules hook in). Give it a `CONFIG_AB_FEATURE_MYTHING` switch in `main/Kconfig.projbuild` and `app/features.h`, wrap the .c file in `#if`, and put inert stand-ins in the header
   if other code calls it. Add the name to `/status.features` in `web/ota_http.c`.
4. Add a card to `web/index.html` (cards that belong to a feature carry `data-feature="mything"`; controls that need a login carry the class `auth`).
5. Test it with the microphone like the others (`tools/`), and note the result in `docs/`.

## Where to read next

`app/main.c` (startup, 90 lines) -> `audio/media.h` and `audio/tone.c` (the heart) -> `audio/dac.c` (talking to the amp) -> `sources/stream.c` (the simplest source) ->
`sources/clip.c` (decoding) -> `web/ota_http.c` (HTTP, login, status) -> `sources/bluetooth.c` (the most involved source) -> `docs/*.md` for the findings behind the numbers.
