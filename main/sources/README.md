# sources/

Everything that produces sound or feeds it: `clip.*` (MP3/WAV from flash), `player.*` (transport buttons), `ambient.*` (rain plus random events), `radio.*` (internet radio),
`synth.*` (OSC synthesizer), `stream.*` (TCP), `netaudio.*` (VBAN and Scream), `bluetooth.*` (A2DP), `storage.*` (the clip file system), `minimp3.h` (the MP3 decoder, CC0).
Each source gets the main channel with `media_begin()` and gives it back with `media_finish()`. To add one, see "How to add a new sound source" in `docs/architecture.md`.
