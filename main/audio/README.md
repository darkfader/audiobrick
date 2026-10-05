# audio/

The audio engine. `media.*` holds the ring buffers that sources write into, `tone.*` is the mixer and I2S output task, `dac.*` drives the TAS5825M amplifier (volume, mute,
Hi-Z, power-down), `speaker_limits.*` turns the speaker profile into a maximum volume, `eq.*` is the parametric equaliser. See `docs/architecture.md`.
