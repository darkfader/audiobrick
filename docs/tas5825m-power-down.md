# TAS5825M idle power-down on the Esparagus Audio Brick (ESP32): what was measured

Findings from the firmware in this repository (v1.1.0), tested on a real Esparagus Audio Brick (ESP32, rev unknown, C1-like) with
two 6 ohm speakers and a USB microphone. Written so that others using the TAS5825M, or this board, can reuse them.
Code: `main/dac.c`. Tests: `tools/amp_power_test.py`, `tools/wake_test.py`.

## The three idle states

| State | How | What the chip does | Wake-up |
|---|---|---|---|
| Active, muted | `CTRL_STATE` (reg 0x03) = 0x0B (play + mute bit 0x08) | output stage running, silent | immediate |
| Hi-Z | reg 0x03 = 0x02 | output stage high impedance, chip and registers still alive, I2C answers | write 0x0B, then 0x03 |
| Powered down | PWDN pin (GPIO33 on this board) low | chip off; **registers are lost; I2C does not answer** | PWDN high, 10 ms, full init sequence (about 30 ms), volume rewritten |

The firmware goes muted -> Hi-Z after `hiz_after_s` (default 20 s) and Hi-Z -> PWDN low after `off_after_s` (default 600 s). Both can be
changed with `POST /power` (0 = never).

## What was verified

- **The chip really shuts down.** With PWDN low, an I2C probe of address 0x4C gets no acknowledge; when awake it does
  (`GET /power` shows `i2c_ack`). Reads of the fault and PVDD registers are skipped in that state (`/status` shows `pvdd_v` -1).
- **Wake-up costs nothing audible.** A sound that starts from the awake, Hi-Z and powered-off states becomes audible after the same
  time (differences within about 12 ms, measurement noise) and lasts the same length (997 ms of 1000 ms through the stream port, 1073-1088 ms
  through the test-tone path in every state). Reason: the power-up takes about 30 ms and runs while the firmware's mixer is still fading in
  (100 ms ramp), and the I2S clock keeps running the whole time (the TAS5825M derives its clock from BCK/WS; whether stopping I2S during power-down would be a problem was not tested).
- **No audible click** at muted -> Hi-Z, Hi-Z -> off, or at wake-up (the owner heard none, and 2-20 kHz energy around each transition was not above that of a normal
  tone start, microphone at the speaker). Mute first, then Hi-Z, then PWDN; do not drop PWDN while the amp is playing.
- After a power cycle everything (volume, BTL mode, analog gain) must be written again. The firmware keeps the wanted volume and re-applies it.

## Caveats

- **Supply current was not measured** (no meter on the supply). The "about 1 mW" style figures belong to the datasheet, not to this test. Measure
  it yourself before claiming a number for this board; the board's own regulators, the W5500 and the LED are still powered.
- Only one board, with a quiet room only for the last runs; typing and fans near the microphone ruin the 2-20 kHz click measurement.
- The click test cannot tell a very quiet tick under about the room noise from silence.
- Fault pins: FAULTZ and WARNZ are only meaningful while the chip is powered; the firmware reports "no fault" while it is off.

## Related board notes (from the same bring-up)

- Flash is 16 MB, not 8 MB as in the upstream YAML; only 4 MB of the 8 MB PSRAM is mapped on the ESP32.
- TAS5825M I2C address 0x4C; PVDD ADC at reg 0x5E, V = raw / 8.428.
- The CH340 auto-reset into download mode did not work on this board (manual BOOT/EN needed), cause unknown.
- The W5500 runs in MAC-raw mode under ESP-IDF, so multicast reception has to be enabled on it (`ETH_CMD_S_ALL_MULTICAST`, argument is a pointer to a bool) or mDNS and
  Scream multicast are silently dropped.
