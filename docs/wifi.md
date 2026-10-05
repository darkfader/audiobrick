# Wi-Fi client (second network interface)

Ethernet stays the preferred transport. Wi-Fi is a fallback: the Brick can join a remembered network and then has two addresses (both DHCP). The wired one is used first for outgoing traffic.

## Use

- Page card "Wi-Fi", or the endpoints in `main/net/wifi.h` (all need the login): `GET /wifi`, `POST /wifi?on=0|1`, `POST /wifi/scan` then `GET /wifi/scan`, `POST /wifi/add` (body: name, newline, password), `POST /wifi/forget` and `POST /wifi/connect` (body: name).
- Up to 8 networks are remembered in flash (`wifi_nets`); the password is never sent back. While Wi-Fi is on and not connected the Brick scans every 8 s (30 s after repeated failures) and joins the strongest remembered network; a link that drops is picked up again the same way.
- Wi-Fi can be switched on with no network stored, which is how you scan first. It is off by default (a first start or a build without stored settings).
- Build switch: `AB_FEATURE_WIFI` (`idf.py menuconfig`, Audio Brick features).

## What it costs

- Image: about +300 KB, which is why the OTA slots are 3 MB since 2026-10-05 (the partition change needs one serial flash).
- Internal RAM: about 50 KB while on (free 82 KB -> 32 KB). The Bluetooth stack takes its share too. Settings that keep it small are in `sdkconfig.defaults` (small buffers, driver and lwIP allocations in PSRAM first, no access-point mode, no enterprise authentication, no 802.11n aggregation). Dynamic TX buffers cannot be used: they need `SPIRAM_IGNORE_NOTFOUND`, which conflicts with the buffers we keep in PSRAM.

## Memory measures this needed (they help without Wi-Fi too)

- The MP3 decoder task stacks (clips, radio) live in PSRAM. A task with a PSRAM stack must never touch flash (IDF asserts `esp_task_stack_is_sane_cache_disabled` and the Brick restarts), so `clip_play_slot` reads the clip (up to 1.5 MB) into PSRAM first, in the calling task, and the decoder plays from memory. Larger files keep an internal stack.
- The decoders' work buffers (decoder state, input, output, resampler: about 28 KB each) are taken from PSRAM, so two decoders at once cost almost no internal RAM.
- Measured with Wi-Fi on and the Bluetooth stack loaded: idle 53 KB free, radio low point 32 KB (it was 3 KB), two clip decoders 42 KB.
- Decoder tasks are pinned to core 1; core 0 belongs to the Wi-Fi and Bluetooth stacks.
- The firmware is built with `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`). At `-Og` the audio mixer used 3.9 ms of its 5.3 ms block budget even when idle.

## Known limit

With Wi-Fi on and a Bluetooth stream playing, a clip or announcement mixed on top can make the music stutter (25 to about 200 short dropouts per clip in the tests; none with Wi-Fi off, none with Bluetooth and Wi-Fi alone for 5 minutes, none with clips and Wi-Fi without a Bluetooth stream). Ruled out: free memory, the amp supply voltage (PVDD steady), flash reads, the decoder sharing a core with the Bluetooth stack. The audio block times (`GET /diag/audio`) show spikes even when idle, which points at contention for the external RAM bus (the Wi-Fi and Bluetooth stacks, the decoders and the mixer all use PSRAM). Not proven. Ideas not tried: keep the mixer's ring buffers in internal RAM, fewer PSRAM allocations in the radio stacks, other Wi-Fi/Bluetooth coexistence settings. Until then: keep Wi-Fi off when you use Bluetooth with clips.

## Diagnostics

`GET /diag/audio` (login): worst-case time in microseconds of the audio mixer's 256-frame blocks (reading the channels, mixing, EQ, total; the budget is 5333) and how many blocks took over 4 ms, since the last request. Each request clears the numbers.
