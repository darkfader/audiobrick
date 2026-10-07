# Home Assistant

The Brick has a Home Assistant integration in `custom_components/audiobrick/` (a "custom component", installable with HACS or by copying the folder). It talks to the Brick's
HTTP API over the local network; there is nothing to install on the Brick. Status: written 2026-10-07 and tested end to end against a real Brick with a throwaway Home Assistant 2026.10 (in WSL: the setup dialog with a wrong and a right password, 17 entities with correct values,
play a clip, announce from a URL, stop, sleep timer, the error for a missing audio file). It has **not yet been installed in a long-running Home Assistant** (the owner's runs 2026.6.4), and zeroconf discovery was not exercised.

## Install

- **HACS:** HACS -> Integrations -> three dots -> Custom repositories -> add `https://github.com/darkfader/audiobrick` as type *Integration* -> install "Esparagus Audio Brick" -> restart Home Assistant.
- **By hand:** copy the folder `custom_components/audiobrick` into the `custom_components` folder of your Home Assistant configuration and restart.

Then Settings -> Devices & services -> Add integration -> "Esparagus Audio Brick". The Brick shows up by itself when Home Assistant sees it on the network (it announces itself
as "Esparagus Audio Brick" over mDNS); otherwise enter its name (`audiobrick.local`) or IP address. You also need the Brick's **web password** (the 32 characters printed on the serial
console at the first start, changeable on the Brick's web page). The password is stored in Home Assistant's configuration and sent as an `X-Token` header over plain HTTP, so use this on a trusted network only.

## What you get (one device per Brick)

| Entity | What it does |
|---|---|
| **Media player** | play / pause / stop / next / previous over the stored clips, volume (the amp volume, never above the speaker-profile limit), browse and play a clip (media browser or `media_player.play_media` with the clip name), play an internet radio stream (`media_id` = an http(s) address of an MP3 stream), **announcements** (below). |
| Sensors | supply voltage (PVDD), amplifier state (awake / output off / powered down), estimated sound level, stream buffer, dropouts, Bluetooth device; disabled by default: uptime, free memory, last restart reason |
| Binary sensors | amplifier fault, amplifier warning, safe mode, Bluetooth connected, Bluetooth pairing window open |
| Switches | ambient sound scene, Bluetooth on/off |
| Buttons | stop, open the Bluetooth pairing window (2 minutes), disconnect the Bluetooth device |
| Service `audiobrick.sleep_timer` | stops everything after N minutes (0 cancels); `device_id` optional |

Entities only exist for features that are built into the firmware (`/status.features`): a minimal build has no Bluetooth entities, no ambient switch, and so on.

## Announcements and text to speech

The media player supports `announce`: Home Assistant fetches the audio (an MP3 or 16-bit WAV, **at most 400 KB**, about 25 seconds of speech), the integration uploads it to the Brick, and the Brick plays it
over whatever is playing while the music is lowered (ducking; the amount is the "duck" setting on the Brick's page). Examples:

```yaml
# text to speech
action: tts.speak
target:
  entity_id: tts.home_assistant_cloud      # any TTS entity you have
data:
  media_player_entity_id: media_player.audio_brick
  message: "The washing machine is done."
```

```yaml
# a stored clip as a doorbell, over the music
action: media_player.play_media
target:
  entity_id: media_player.audio_brick
data:
  media_content_type: music
  media_content_id: door_close_1.mp3
```

```yaml
# sleep timer from a button or an automation
action: audiobrick.sleep_timer
data:
  minutes: 45
```

Quiet hours and the alarm are configured on the Brick's web page (they run on the Brick itself, also when Home Assistant is down).

## Limits

- Polling every 5 seconds; state changes made on the Brick show up within that time.
- One announcement at a time; an announcement longer than 400 KB is refused with an error.
- The old YAML package `homeassistant/audiobrick.yaml` (REST sensors and commands) predates this integration and is kept only as an example for people who prefer YAML; it was never tested against a real Home Assistant.
- Not done yet: discovery without the Brick's `mac` field needs firmware 1.1.0 built on or after 2026-10-07 (older builds can still be added by address).
