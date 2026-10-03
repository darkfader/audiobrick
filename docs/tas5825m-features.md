# TAS5825M Practical Reference (ESP32 / Esparagus Audio Brick)

Scope: hobbyist reference for the TI TAS5825M digital-input closed-loop class-D amp with DSP, used on an
ESP32 board (Esparagus Audio Brick: I2C 0x4C, I2S 48 kHz, PWDN on a GPIO, FAULTZ/WARNZ pins, BTL stereo or
PBTL mono, PVDD roughly 5-26 V).

Confidence tags used throughout:

- **[DS]** read directly from the TI datasheet SLASEH7H (rev H, Jan 2023)
- **[AN]** from TI app note SLAA786A "TAS5825M Process Flows" (Jul 2018)
- **[DRV]** from open-source driver source/README (mrtoy-me/esphome-tas58xx, sonocotta/esp32-tas5805m-dac, sonocotta/esparagus-media-center)
- **[UNV]** unverified / inferred / could not be confirmed. Do not rely on it without checking.

Datasheet note: the PDF text extraction was good for the register map, but some tables (OV/UV/OT thresholds,
abs-max columns, the Fsw/bandwidth grid, the serial-port clock-ratio grid) came out column-scrambled. Affected
values are flagged [UNV]. The DSP memory-map tables in the app note were also scrambled, so DSP parameter
addresses are deliberately not listed.

---

## 1. Feature overview

### 1.1 Power stage and supply

| Item | Value | Source |
|---|---|---|
| Package | 32-pin VQFN (RHB) 5x5 mm, exposed thermal pad | DS |
| PVDD (power stage) | 4.5 V to 26.4 V recommended, abs max 32 V | DS |
| DVDD / I/O | 1.62 V to 3.63 V (1.8 V or 3.3 V), abs max 3.9 V | DS |
| Internal rails | GVDD 5 V, AVDD 5 V (from PVDD), VR_DIG 1.5 V. Do not load these pins | DS |
| RDS(on) | 90 mOhm headline figure (the electrical table lists a higher FET+metallization value) | DS |
| Efficiency | ">90 %"; quiescent PVDD current <20 mA at 12 V (headline) | DS |
| Output power, stereo BTL | 2x30 W (8 ohm, 24 V, 1 % THD+N), 2x38 W (10 %) | DS |
| Output power, mono PBTL | 1x53 W (4 ohm, 22 V, 1 %), 1x65 W (10 %); 50/60 W into 3 ohm at 19 V | DS |
| Minimum load | BTL 3.2 ohm min (4 ohm nominal); PBTL 1.6 ohm min (2 ohm nominal) | DS |
| Output inductor | 1 uH min under short, 4.7 uH nominal at <=12 V, 10 uH above 12 V (384 kHz) | DS |
| THD+N | ~0.03 % at 1 W / 1 kHz | DS |
| SNR / dynamic range | ~108-111 dB A-weighted (PVDD dependent) | DS |
| Idle channel noise | 35 uVrms (headline) | DS |
| Crosstalk | -100 dB at 1 kHz | DS |
| Offset voltage | +/-7.5 mV max (BTL, zero input) | DS |
| Thermal | RthetaJA 24.1 C/W on the 4-layer EVM board | DS |

Notes:

- The Audio Brick README quotes "about 2x40 W at 4 ohm" [DRV]. That is a board-level claim. The datasheet's 2x30 W
  is at 8 ohm / 24 V. Treat 40 W/ch sustained as unverified.
- The "5-26 V" range in the brief is slightly inside the datasheet range (4.5-26.4 V). PVDD UV/OV thresholds were
  scrambled in extraction [UNV exact values].
- Datasheet front page says "38-W stereo" while the product headline in the request says 2x30 W; both are in the
  datasheet (30 W at 1 % THD+N, 38 W at 10 %).

### 1.2 Switching frequency, loop bandwidth, modulation

Switching frequency: DEVICE_CTRL_1 (0x02) bits [6:4] FSW_SEL [DS]:

| FSW_SEL | Fsw |
|---|---|
| 000 | 384 kHz (default) |
| 010 | 480 kHz |
| 011 | 576 kHz |
| 100 | 768 kHz |
| 001, 101, 110, 111 | reserved |

Modulation: DEVICE_CTRL_1 bits [1:0] DAMP_MOD [DS]: 00 = BD, 01 = 1SPW, 10 = Hybrid.

- **BD**: filterless-capable with short speaker wires, lowest THD, highest idle current.
- **1SPW**: "low idle current" mode, only one output switches for most of the audio cycle (~17 % modulation at idle).
  Better efficiency, slight THD penalty, needs more care with the output filter. Driver benchmark note: 1SPW wins above
  about 15 V, BD preferred at lower voltages [DRV, sonocotta README].
- **Hybrid**: DSP senses signal level and adjusts PWM duty versus PVDD. Ultra-low idle current with BD-like audio quality,
  aimed at battery products. **Requires a process flow that supports Hybrid modulation** (selected in TI's PPC3 tool) [DS].
  The ESPHome component only exposes BD and 1SPW enums [DRV], so hybrid is probably not reachable with the ROM default
  flow [UNV].

Class-D loop bandwidth: ANA_CTRL (0x53) bits [6:5]: 00 = 100 kHz, 01 = 80 kHz, 10 = 120 kHz, 11 = 175 kHz [DS].

- Register text: at 384 kHz, 100 kHz BW for best audio performance; at 768 kHz, 175 kHz BW.
- Table 9-3 rule of thumb: Hybrid/1SPW want Fsw >= 4.2 x BW; BD wants Fsw >= 3 x BW. Same Fsw, higher BW gives better THD+N.
  The exact per-cell grid of allowed combinations was scrambled in extraction [UNV].
- Inductor minimum scales as (384 kHz / Fsw) x L(384 kHz) [DS].

### 1.3 BTL / PBTL

- DEVICE_CTRL_1 bit 2 DAMP_PBTL: 0 = BTL stereo, 1 = PBTL mono [DS].
- In PBTL the input is the **left slot of I2S/TDM** [DS]. If your source is on the right, reroute via SAP_CTRL3 or the input mixer.
- Merge outputs before the inductor (pre-filter PBTL) or after (post-filter, two extra inductors but smaller parts) [DS].
- **Cycle-by-cycle current limiting works only in BTL, not PBTL** [DS]. Hardware over-current shutdown still applies.
- Set the mode while in Hi-Z / before Play [UNV: standard practice, not stated verbatim].

### 1.4 Analog gain / output swing

Total gain is digital volume (DSP) plus a 32-step analog gain [DS]:

- AGAIN (0x54) bits [4:0]: 0 = 0 dB = **29.5 V peak** at 0 dBFS; each step -0.5 dB; 31 = -15.5 dB = **4.95 V peak**.
- Peak(V) = 29.5 x 10^(-n x 0.5 / 20). Examples: n=0 29.5 V, n=1 27.85 V, n=10 16.6 V, n=20 9.3 V, n=31 4.95 V.
- The digital boost stays at 0 dB for all AGAIN settings. 29.5 V is a *scale*, not a rail: actual output clips near PVDD.
  Pick AGAIN so 0 dBFS roughly matches PVDD for sensible headroom [DS rationale].
- Gain is closed-loop, constant across supply voltage; PSRR 72 dB at 1 kHz.
- Both drivers expose analog gain as 0 to -15.5 dB in 0.5 dB steps [DRV].

### 1.5 Sample rates, interface formats, word lengths

- Supported Fs: 32, 44.1, 48, 88.2, 96, 176.4, 192 kHz with automatic detection [DS]. An SRC converts to the DSP's
  48 kHz or 96 kHz architecture [DS].
- Formats: SAP_CTRL1 (0x33) bits [5:4]: 00 I2S, 01 TDM/DSP, 10 right-justified, 11 left-justified [DS]. **Default I2S, 24-bit.**
- Word length bits [1:0]: 00 = 16, 01 = 20, 10 = 24 (default), 11 = 32 bit [DS].
- Data offset (TDM slots): SAP_CTRL1 bit 7 (MSB) + SAP_CTRL2 (0x34) = up to 512 SCLK [DS].
- Accepted SCLK:LRCLK ratios 32 FS to 512 FS, BCLK 256 kHz to 50 MHz [DS]. The per-format/per-rate grid in Table 9-1
  was scrambled [UNV]. 48 kHz with 64 fs (3.072 MHz) is a standard, safe choice.
- SCLK polarity invert: I2S_CTRL (0x31) bit 5 [DS].
- SDOUT: a GPIO can output the stream, pre- or post-DSP (SDOUT_SEL 0x30 bit 0), for monitoring, echo cancel or a sub channel [DS].
- Changing Fs on the fly: halt SCLK/LRCLK at least 100 us first [DS].
- Data is two's complement, MSB first, up to 32 bits [DS].

### 1.6 Clocking without MCLK, PLL, clock errors

- Three-wire interface (SCLK, LRCLK, SDIN), **no MCLK needed**; an internal PLL derives DSP and DAC clocks from SCLK [DS].
- FS_MON (0x37) reports detected Fs and BCK_MON (0x38) the SCLK ratio [DS].
- If clocks stop, outputs go Hi-Z, a clock fault is flagged, and the device **auto-recovers** to the previous state when
  clocks return, with no DSP reload [DS].
- CLOCK_DET_CTRL (0x29) can ignore individual detections: PLL overrate (bit 6), SCLK range (5), FS error (4),
  SCLK-vs-LRCK ratio (3), SCLK missing (2) [DS]. The ESPHome component has a CLOCK_FAULT exclude/ignore option [DRV].
- CLKDET_STATUS (0x39) [5:0]: FS valid, SCLK valid, SCLK missing, PLL locked, PLL overrate, SCLK over/under-rate [DS].

### 1.7 Digital volume, ramp, auto-mute, soft mute

- DIG_VOL (0x4C): 8-bit, **one register for both channels**: 0x00 = +24 dB, 0x30 = 0 dB (default), 0xFE = -103 dB,
  0xFF = mute; 0.5 dB steps [DS]. The ESPHome component also exposes separate left/right volume [DRV]; how those map
  onto DSP gain coefficients is [UNV].
- DIG_VOL_CTRL1 (0x4E, default 0x33): ramp-down speed [7:6] and step [5:4]; ramp-up speed [3:2] and step [1:0].
  Speed = update every 1, 2 or 4 sample periods, or 11 = instant. Step = 4, 2, 1 or 0.5 dB [DS].
  Default is every sample, 0.5 dB steps.
- DIG_VOL_CTRL2 (0x4F, default 0x30): "emergency" fast ramp-down on clock error / power loss [DS].
- Soft mute: DEVICE_CTRL2 (0x03) bit 3 ramps both channels pop-free [DS].
- Auto-mute: AUTO_MUTE_CTRL (0x50, default 0x07: both channels enabled, mute together only) and AUTO_MUTE_TIME (0x51):
  11.5 ms, 53 ms, 106.5 ms, 266.5 ms, 0.535 s, 1.065 s, 2.665 s, 5.33 s of consecutive zero samples, quoted at 96 kHz and
  scaling with Fs (roughly double at 48 kHz) [DS]. AUTOMUTE_STATE (0x69) reads status; flags can be routed to a GPIO.
- A GPIO can be configured as a MUTEZ input (GPIO_INPUT_SEL 0x64 bits [1:0]) that forces outputs Hi-Z [DS].

### 1.8 Mixer, EQ, DRC, AGL, clipper, DSP

From SLAA786A [AN] unless stated. The DSP runs one of several **process flows**. DSP_PGM_MODE (0x40) defaults to 0x01
(ROM mode 1); 0 = RAM mode, 1-3 = ROM modes [DS]. Which app-note flow each ROM mode corresponds to is [UNV].
Flows listed in the app note:

1. Base/Pro 96 kHz 2.0 (15 BQ, 3-band DRC, DPEQ, spatializer, clipper, crossbar)
2. 2-band DRC + AGL, 96 kHz 2.0
3. 3-band DRC + AGL, 96 kHz 2.0
4. SmartAmp (excursion/thermal limiter, SmartBass), 96 kHz 2.0
5. SmartAmp LookAhead, 48 kHz 2.0
6. FIR (128 taps, phase delay up to 50 samples) with woofer/tweeter crossover, 48 kHz 2.0
7. Base/Pro 48 kHz 2.0
8. Housekeeping 2.0 (SRC + crossbar, lowest power)
9. Base/Pro 48 kHz 1.1
10. SmartAmp 48 kHz 1.1
11. 96 kHz 1.1
12. Base/Pro 48 kHz 2.1
13. SmartAmp 48 kHz 2.1

Blocks (flow dependent):

- **Input mixer**: four gains (L->L, R->L, L->R, R->R) with sign invert, 9.23 fixed point; default identity (0x00800000 = 1.0).
  Gives mono sum, swap, single-channel select, polarity inversion.
- **15 biquads per channel (30 total)**, cascaded direct-form-1, coefficients in **5.27** format, five per BQ
  (B0, B1, B2, A1, A2) normalized by a0 [AN]. Left/right can be ganged. Coefficients live in **Book 0xAA**; all five of a BQ
  must be written sequentially [DS]. Defaults are pass-through (B0 = 0x08000000).
  The ESPHome component builds a 15-band graphic EQ (20 Hz-16 kHz, +/-15 dB, 30 gain entities) and presets on top [DRV].
- **DC block**: a "Bypass DC block" flag exists in DSP parameter memory (Book 0x78, page 0x0B) with default 0 [AN], which I read
  as the DC-blocking filter being active by default. Cutoff frequency not found in the material read [UNV].
- **Volume** (DSP): alpha-filter smoothed, click-free [AN].
- **Spatializer** (stereo widening; bandpass-driven, advised not below ~300 Hz) and **DPEQ** (level-dependent EQ crossfade) [AN].
- **DRC**: 3-band (4th-order crossover) or 2-band; per band 3 compression regions (thresholds, slopes, offsets) and
  separate energy/attack/release constants [AN].
- **AGL**: full-band automatic gain limiter after the DRC in the relevant flows [AN].
- **Clipper / "THD boost"**: lets you clip digitally earlier than the supply rails, deliberately raising THD to bound the swing
  or gain loudness; fine L/R volumes sit beside it [AN].
- **Output crossbar**: routes DSP L/R into amplifier outputs and I2S SDOUT with gains [AN].
- **Level meter**, **SmartAmp** (excursion/thermal limiter, SmartBass, anti-clipper) [AN].
- **PVDD tracking** (dynamic headroom) and **thermal foldback** use the PVDD sense ADC and 4-level temperature sensor;
  details are in the separate "Advanced Features" app note, which I did not read [DS pointer].

Practical caveat: the DSP is normally configured with coefficient blobs generated by TI's PurePath Console 3 (PPC3), or by
reproducing the memory map by hand.

### 1.9 Spread spectrum and EMI features

- Triangle spread spectrum: SS_CTRL0 (0x6B) bit 0 SS_TRI_EN; random SS: bit 1 SS_RDM_EN [DS].
  SS_CTRL1 (0x6C) bits [3:0] select triangle frequency (24 kHz or 48 kHz) and range (5/10/20/25 %); bits [6:4] random dither [DS].
- TI example for 384 kHz Fsw: `0x6B = 0x03`, `0x6C = 0x03` -> 24 kHz triangle, range 25 % (336-432 kHz) [DS]. By the field map,
  0x03 in 0x6B enables both triangle and random; 0x01 would be triangle only [UNV which is preferable].
- Manual ramp/SS tuning in 0x6D-0x6F (SS_CTRL2-4) [DS].
- Channel-to-channel 180-degree PWM phase shift: ANA_CTRL (0x53) bit 0 (0 = out of phase, default) [DS].
- Multi-device PWM phase sync (up to 4 devices, 45-degree steps) via PHASE_CTRL (0x6A), by I2S-start or by GPIO [DS].
- TI recommends identical Fsw and SS settings across devices, set before entering Play [DS].

### 1.10 Protection, fault and warning reporting

| Condition | Behaviour | Reporting | Source |
|---|---|---|---|
| Over-current shutdown (OCSD) | Channel shuts down in <100 ns on severe short; restart via I2C | CHAN_FAULT bits 1,0; FAULTZ | DS |
| Cycle-by-cycle current limit | Terminates PWM pulses, like soft clipping. **BTL only** | GLOBAL_FAULT2 bits 2:1; WARNING bits 5:4 | DS |
| DC detect | Output Hi-Z; FAULTZ low | CHAN_FAULT bits 3,2 | DS |
| PVDD under-voltage | Hi-Z; auto-recovers | GLOBAL_FAULT1 bit 0 | DS |
| PVDD over-voltage | Hi-Z; auto-recovers | GLOBAL_FAULT1 bit 1 | DS |
| Clock fault | Hi-Z; auto-recovers when clocks return | GLOBAL_FAULT1 bit 2 | DS |
| Over-temperature shutdown | Hi-Z; auto-recovery optional (MISC_CONTROL bit 4) | GLOBAL_FAULT2 bit 0 | DS |
| Over-temperature warning | 4 levels: 112, 122, 134, 146 C | WARNING bits 0-3 (0x73) | DS |
| OTP CRC / BQ write / EEPROM load error | Status only | GLOBAL_FAULT1 bits 7,6,5 | DS |

- Latched faults clear by writing 1 to FAULT_CLEAR (0x78) bit 7 [DS]. Latching of CBC fault/warning, clock fault, OTSD and OTW is
  controlled by PIN_CONTROL2 (0x75, default 0xF8, all latched) [DS].
- Individual faults can be masked from the FAULTZ/WARNZ pins via PIN_CONTROL1 (0x74) and PIN_CONTROL2 (0x75) [DS].
- OC error peak current is about 7.5 A per the inductor-selection text [DS]. PVDD OV/UV and OTSD threshold numbers were scrambled [UNV].
- **Thermal foldback**: from OTW level 1 to 4 an internal AGL gradually reduces digital gain (more at higher levels) and restores it on
  cooling; attenuation and attack/release are programmable in PPC3 [DS].
- The ESPHome component exposes 12 fault binary sensors and a faults-cleared counter [DRV].

### 1.11 GPIOs

- GPIO0/1/2 (pins 9, 10, 11): direction via GPIO_CTRL (0x60); function via GPIO0_SEL/GPIO1_SEL/GPIO2_SEL (0x61/0x62/0x63) [DS]:
  0000 off (low); 0010 user value (GPIO_OUT 0x65); 0011 auto-mute (both); 0100 auto-mute L; 0101 auto-mute R; 0110 clock invalid;
  1000 **WARNZ**; 1001 SDOUT; 1011 **FAULTZ**; 1100 SPI CLK; 1101 SPI PICO.
- Inputs via GPIO_INPUT_SEL (0x64): MUTEZ, RESETZ, PHASE_SYNC, SPI POCI, each assignable to GPIO0/1/2 [DS].
- GPIO_OUT_INV (0x66) inverts outputs [DS]. Pin text says CMOS or open-drain output for WARNZ/FAULTZ.
- Front-page diagram labels GPIO0 FAULT, GPIO1 MUTE, GPIO2 SDOUT as a typical use [DS].
- Brick wiring (ESP32-S3 variant): FAULTZ = ESP GPIO18, WARNZ = ESP GPIO04 [DRV]. Which TAS GPIO each connects to, and the matching
  GPIOx_SEL values, must be taken from the Brick schematic [UNV].
- DSP RAM can also be loaded from an external SPI EEPROM via GPIOs (EEPROM_* registers 0x55-0x5B) [DS].

### 1.12 I2C addressing and bus

7-bit address = 0b10011 + 2 bits set by ADR pin resistor to GND [DS]:

| ADR resistor to GND | 7-bit addr | 8-bit write |
|---|---|---|
| 0 ohm | **0x4C** | 0x98 |
| 1 kohm | 0x4D | 0x9A |
| 4.7 kohm | 0x4E | 0x9C |
| 15 kohm | 0x4F | 0x9E |

- 100 kHz and 400 kHz, sequential read/write with auto-increment [DS].
- **Paged memory**: register 0x00 on every page selects the page; register 0x7F on page 0 selects the **book**.
  Switch: write 0x00 to reg 0x00, write book to reg 0x7F, write page to reg 0x00. All documented control registers are Page 0 / Book 0 [DS].
- Optional CRC (reg 0x7E) and XOR (Book 0x8C page 0 reg 0x7D) write checksums to verify config loads [DS].
- Digital inputs are DVDD-referenced (abs max DVDD + 0.5 V) [DS]. Level-shift if the ESP32 is 3.3 V and DVDD is 1.8 V.

### 1.13 Power states and hardware pins

- DEVICE_CTRL2 (0x03) bits [1:0]: 00 Deep Sleep, 01 Sleep, 10 Hi-Z, 11 Play. POWER_STATE (0x68) reports the actual state [DS].
  - Deep sleep: I2C alive, DSP keeps running; DVDD 0.82 mA, PVDD ~12 uA at 13.5 V.
  - Sleep: I2C, digital core, DSP memory and 5 V analog LDO alive; DVDD 0.87 mA, PVDD ~7.3 mA at 13.5 V.
  - Hi-Z: only output drivers off; PVDD ~10.7 mA at 13.5 V.
  - Play: ~29.5 mA PVDD (13.5 V, Hybrid, 10 uH + 0.68 uF, no load); ~20.5 mA with 22 uH. DVDD ~14.8-25.5 mA by flow.
  - Shutdown (PDN low): DVDD 7.4 uA, PVDD 7.8 uA, regulators off; **DSP and registers are lost**.
- PDN: active-low power-down, DVDD-referenced [DS].
- Datasheet start-up [DS]: set ADR; power PVDD and DVDD in any order; PDN high; wait >= 5 ms; start SCLK/LRCLK; with clocks
  stable set **Hi-Z** and enable the DSP (clear DIS_DSP, 0x03 bit 4); wait >= 5 ms; load DSP coefficients; set **Play**.
- Shutdown [DS]: set Hi-Z (or PDN low); wait >= 6 ms (depends on Fs and ramp settings); remove supplies.
- RESET_CTRL (0x01): bit 0 resets control-port registers, bit 4 resets the full digital core (also clears DSP RAM) [DS].

---

## 2. Register table (Page 0, Book 0 unless noted)

"Src": **DS** = datasheet register map (Section 9.6); **DRV** = also defined in the ESPHome driver header; **AN** = app note;
**UNV** = unverified. Defaults as printed in the datasheet.

| Addr | Name | Purpose / key fields | Default | Src |
|---|---|---|---|---|
| 0x00 | PAGE_SEL | Page select (on every page) | 0x00 | DS, DRV |
| 0x7F | BOOK_SEL | Book select (write from page 0) | 0x00 | DS, DRV |
| 0x01 | RESET_CTRL | bit4 RST_DIG_CORE, bit0 RST_REG (write-clear) | 0x00 | DS |
| 0x02 | DEVICE_CTRL_1 | [6:4] FSW_SEL, [2] DAMP_PBTL, [1:0] DAMP_MOD | 0x00 | DS, DRV |
| 0x03 | DEVICE_CTRL2 | [4] DIS_DSP, [3] MUTE, [1:0] CTRL_STATE | 0x10 per field table (datasheet heading prints "00x10") | DS, DRV |
| 0x0F | I2C_PAGE_AUTO_INC | Page auto-increment disable (non-zero books) | 0x00 | DS |
| 0x28 | SIG_CH_CTRL | SCLK ratio config, FS mode select (0 = auto detect) | 0x00 | DS |
| 0x29 | CLOCK_DET_CTRL | Ignore PLL / SCLK-range / FS / ratio / missing detections | 0x00 | DS |
| 0x30 | SDOUT_SEL | SDOUT post- or pre-DSP | 0x00 | DS |
| 0x31 | I2S_CTRL | [5] SCLK_INV | 0x00 | DS |
| 0x33 | SAP_CTRL1 | [7] shift MSB, [5:4] format, [3:2] LRCLK pulse, [1:0] word length | 0x02 | DS |
| 0x34 | SAP_CTRL2 | Data offset LSBs | 0x00 | DS |
| 0x35 | SAP_CTRL3 | [5:4] left DAC path (00 zero, 01 L, 10 R); [1:0] right DAC path (00 zero, 01 R, 10 L) | 0x11 | DS |
| 0x37 | FS_MON | Detected Fs, SCLK ratio high bits | 0x00 | DS, DRV |
| 0x38 | BCK_MON | Detected SCLK ratio low bits | 0x00 | DS, DRV |
| 0x39 | CLKDET_STATUS | [5:0] FS/SCLK/missing/PLL lock/overrate flags | 0x00 | DS |
| 0x40 | DSP_PGM_MODE | [2:0] 0 = RAM, 1-3 = ROM modes | 0x01 | DS |
| 0x46 | DSP_CTRL | [4:3] processing rate (input/48/96/192 k), [0] use default ROM coefficients | 0x01 | DS |
| 0x4C | DIG_VOL | Both channels: 0x00 +24 dB, 0x30 0 dB, 0xFE -103 dB, 0xFF mute (0.5 dB/step) | 0x30 | DS, DRV |
| 0x4E | DIG_VOL_CTRL1 | Normal ramp up/down speed and step | 0x33 | DS |
| 0x4F | DIG_VOL_CTRL2 | Emergency ramp-down speed/step | 0x30 | DS |
| 0x50 | AUTO_MUTE_CTRL | [0] L enable, [1] R enable, [2] mute only when both | 0x07 | DS |
| 0x51 | AUTO_MUTE_TIME | [6:4] L, [2:0] R time (11.5 ms ... 5.33 s at 96 kHz) | 0x00 | DS |
| 0x53 | ANA_CTRL | [6:5] loop bandwidth (00 100k, 01 80k, 10 120k, 11 175k); [0] L/R PWM phase | 0x00 | DS, DRV |
| 0x54 | AGAIN | [4:0] analog gain: 0 = 29.5 Vp, 31 = 4.95 Vp, 0.5 dB steps | 0x00 | DS, DRV |
| 0x55 | SPI_CLK | SPI clock config for EEPROM boot | 0x00 | DS |
| 0x56-0x5B | EEPROM_* | EEPROM boot control, read command, start address, status | n/a | DS (names only) |
| 0x5C | BQ_WR_CTRL1 | bit0 marks first coefficient of a BQ being written | 0x00 | DS |
| 0x5E | PVDD_ADC | PVDD (V) = value / 8.428 (223 = 26.45 V, 38 = 4.51 V) | 0x00 | DS |
| 0x60 | GPIO_CTRL | GPIO0..2 output enable (bits 0..2) | 0x00 | DS |
| 0x61 | GPIO0_SEL | Function select (see 1.11) | 0x00 | DS |
| 0x62 | GPIO1_SEL | Function select | 0x00 | DS |
| 0x63 | GPIO2_SEL | Function select | 0x00 | DS |
| 0x64 | GPIO_INPUT_SEL | Input pin routing for MUTEZ, RESETZ, PHASE_SYNC, SPI POCI | 0x00 | DS |
| 0x65 | GPIO_OUT | User-driven GPIO levels | 0x00 | DS |
| 0x66 | GPIO_OUT_INV | Output invert | 0x00 | DS |
| 0x67 | DIE_ID | Chip ID, useful presence check | 0x95 | DS |
| 0x68 | POWER_STATE | 0 DeepSleep, 1 Sleep, 2 HiZ, 3 Play | 0x00 | DS, DRV |
| 0x69 | AUTOMUTE_STATE | bit0 left auto-muted, bit1 right | 0x00 | DS |
| 0x6A | PHASE_CTRL | Multi-device phase select / sync | 0x00 | DS |
| 0x6B | SS_CTRL0 | bit0 triangle SS enable, bit1 random SS enable, manual-mode bits | 0x00 | DS |
| 0x6C | SS_CTRL1 | [3:0] triangle freq/range, [6:4] random dither | 0x00 | DS |
| 0x6D-0x6F | SS_CTRL2-4 | Manual-mode SS tuning | 0xA0, 0x11, 0x24 | DS |
| 0x70 | CHAN_FAULT | b3 CH1 DC, b2 CH2 DC, b1 CH1 OC, b0 CH2 OC | 0x00 | DS, DRV |
| 0x71 | GLOBAL_FAULT1 | b7 OTP CRC, b6 BQ write, b5 EEPROM load, b2 clock, b1 PVDD OV, b0 PVDD UV | 0x00 | DS, DRV |
| 0x72 | GLOBAL_FAULT2 | b2/b1 CBC fault CH2/CH1, b0 OTSD | 0x00 | DS, DRV |
| 0x73 | WARNING | b5/b4 CBC warn, b3..b0 OTW level 4..1 (146/134/122/112 C) | 0x00 | DS, DRV |
| 0x74 | PIN_CONTROL1 | Mask OTSD, DVDD UV/OV, clock, PVDD UV, DC, OC from FAULTZ | 0x00 | DS |
| 0x75 | PIN_CONTROL2 | Latch enables (b7..b3), mask OTW / CBC warn / CBC fault | 0xF8 | DS |
| 0x76 | MISC_CONTROL | b7 latch clock-detect status, b4 OTSD auto-recovery enable | 0x00 | DS |
| 0x77 | CBC_CONTROL | b2 CBC enable, b1 warning, b0 fault | 0x00 | DS |
| 0x78 | FAULT_CLEAR | b7 ANALOG_FAULT_CLEAR (write 1) | 0x00 | DS, DRV |
| 0x7D (Book 0x8C) | XOR checksum | Write checksum, XOR scheme | n/a | DS |
| 0x7E | CRC checksum | Write checksum, CRC-8 | n/a | DS |
| Book 0xAA | BQ coefficients | 15 BQ/channel, 5 coefficients each, format 5.27, written sequentially | pass-through | DS, AN |
| Book 0x78, pages 0x06-0x0B | DSP params | DRC, clipper, DPEQ, spatializer, crossbar, volume, input mixer, DC-block flag, EQ control | flow dependent | AN (byte addresses UNV) |

Discrepancies and cautions:

- The mrtoy ESPHome defs list `TAS58XX_FAULT_CLEAR = 0x78` and a separate `TAS58XX_ANALOG_FAULT_CLEAR = 0x80` [DRV].
  The datasheet puts the analog-fault-clear bit at **bit 7 of register 0x78**, i.e. the value 0x80 written to 0x78 [DS].
  So 0x80 is most likely a value, not a register [UNV: source usage not inspected].
- The datasheet's DEVICE_CTRL2 heading prints "reset = 00x10"; the field table gives DIS_DSP reset = 1 and CTRL_STATE reset = 00 [DS].
- The sonocotta README says volume can be given as 0-124 % or on the native 0-255 scale [DRV]; the exact percent mapping is [UNV].
- GPIO function codes are identical for GPIO0/1/2 in the datasheet.
- Bit-name labels for CHAN_FAULT (CH1 = left, CH2 = right per the field text) are as printed; the bit order of OC vs DC is
  unusual, so verify against a forced fault if you rely on it.

---

## 3. Cool applications for the settings

Always verify with a dummy/resistive load and a scope before connecting anything that is not a normal loudspeaker.

### 3.1 Subwoofer or bi-amp via PBTL and crossover biquads

- **Mono sub at higher power**: DEVICE_CTRL_1 (0x02) bit 2 = 1 (PBTL), outputs paralleled per the datasheet. Min load 2 ohm;
  ~53 W into 4 ohm at 22 V. Feed mono on the *left* I2S slot, or sum with the input mixer / SAP_CTRL3 [DS, AN].
- **Crossover**: program a low-pass (e.g. cascaded second-order sections for LR4) into the BQs in Book 0xAA [DS, AN].
  The sonocotta driver ships sub/satellite EQ presets that already do this [DRV].
- **Bi-amp with two chips**: the Audio Brick dual-DAC prototype sends the same I2S to two TAS5825M for stereo pair, dedicated
  sub, or bi-amp [DRV]. Share Fsw and SS settings and offset PWM phases with PHASE_CTRL (0x6A) [DS].
- Dedicated 2.1 / 1.1 DSP flows exist (flows 9-13) but how to select them from the ROM is [UNV].

### 3.2 Room correction and loudness EQ

- Load measured parametric filters (e.g. from REW) into the 15 BQs per channel; ungang L/R for per-channel correction [AN].
- Loudness contour: low/high shelf BQs updated at runtime as volume changes. The ESPHome component supports automatic or
  manual EQ refresh [DRV].
- DPEQ (flows that include it) does level-dependent EQ in hardware with no CPU involvement [AN].
- When updating a BQ live, write all five coefficients sequentially (BQ_WR_CTRL1 first-coef flag exists; exact glitch behaviour [UNV]).

### 3.3 Night mode with DRC

- 3-band DRC flows compress dynamic range per band so quiet detail stays audible while peaks stay down; pair with a high-pass
  or low-shelf cut to reduce bass energy [AN].
- No-DSP-flow fallback: step DIG_VOL (0x4C) and AGAIN (0x54) down, using the 0.5 dB ramp for slow fades [DS].

### 3.4 Fault logging and telemetry

- Route **FAULTZ** (GPIOx_SEL = 1011) and **WARNZ** (1000) to ESP32 interrupt inputs [DS]. On a FAULTZ interrupt read 0x70-0x73
  (sequential read), timestamp, publish (MQTT / Home Assistant), then write 0x80 to 0x78 to clear [DS].
- Log OTW levels as a thermal time series; read PVDD_ADC (0x5E) for supply monitoring (resolution ~0.12 V per count) [DS].
- Poll POWER_STATE (0x68), FS_MON (0x37) and CLKDET_STATUS (0x39) to confirm I2S is present and the PLL is locked.
- Latching (PIN_CONTROL2) keeps transient faults visible until cleared. The ESPHome component provides 12 fault sensors [DRV].

### 3.5 Thermal-aware limiting

- OTW levels at 112/122/134/146 C feed the internal thermal-foldback AGL (in flows that have it), which reduces digital gain
  progressively [DS].
- Software mirror on the ESP32: poll WARNING (0x73); at level 1 cut DIG_VOL by a few dB, raise on clear. Enable OTSD auto-recovery
  (MISC_CONTROL 0x76 bit 4) so a hard thermal trip restarts instead of waiting for a clear [DS].
- Sustained high power above ~15 V needs a heatsink [DRV].

### 3.6 Speaker protection and driving non-speaker or low-impedance loads (exciters, transducers)

Generic electrical limits of the amp [DS]:

| Limit | Value |
|---|---|
| Min load BTL | 3.2 ohm (4 ohm nominal) |
| Min load PBTL | 1.6 ohm (2 ohm nominal) |
| Over-current | ~7.5 A peak design level; OCSD trips in <100 ns on hard shorts |
| Max swing | roughly PVDD per output leg, up to ~2x PVDD differential peak-to-peak in BTL |
| DC fault | Hi-Z on sustained output DC (threshold [UNV]) |
| Offset | +/-7.5 mV max |

Ways to bound what reaches a delicate or unusual load:

1. Cap swing in analog with AGAIN (0x54), e.g. 0x1F gives 4.95 V peak full-scale regardless of PVDD.
2. Cap further in digital with DIG_VOL (0x4C) attenuation (0x30 = 0 dB, larger values quieter).
3. Use a flow with the clipper / THD boost and AGL / DRC to hard-bound the digital peak and average power [AN]. Tune with a
   resistive load first, as the app note advises.
4. Lower PVDD (for example a 5 V rail) so the rail itself bounds swing.
5. External hardware: series resistance, fuse or PTC, and a series DC-blocking capacitor where DC could harm the load. The amp's
   DC-detect and DSP DC-block are not a substitute.
6. The BTL output is differential: **neither output terminal is ground.** Never tie one to chassis or scope ground.

Loads under 3.2 ohm (BTL) or 1.6 ohm (PBTL) stress the chip and trip current protection.

Human-contact loads (e-stim and similar): the TAS5825M is a general-purpose audio class-D amplifier with no medical or safety
certification. Its outputs swing up to roughly PVDD and can source several amps; there is no isolation and no leakage-current
limiting. Anything connected to a person needs galvanic isolation (isolated supply plus isolating transformer or equivalent),
independent hardware current limiting, and a reviewed safety design. No further guidance is given here.

### 3.7 Battery operation near 5 V

- PVDD must stay >= 4.5 V. A single Li-ion cell dips below that, so use a boost converter or a USB/2S source [DS].
  At 5 V the available power is small; the datasheet gives no 5 V power figure [UNV].
- Use **Hybrid modulation** (DAMP_MOD = 10, needs a Hybrid-capable flow) or 1SPW for low idle current [DS]. Larger inductors cut idle
  current: 22 uH gave 20.5 mA versus 29.5 mA with 10 uH (13.5 V, no load, Hybrid) [DS].
- Idle with **Deep Sleep** (DEVICE_CTRL2 = 00): ~12 uA PVDD and 0.82 mA DVDD, DSP retained, resume by writing Play [DS].
  PDN low is lower still (~7.8 uA) but requires a full reload.
- Read PVDD_ADC (0x5E) for a battery-low warning before UV protection trips [DS].

### 3.8 Hybrid modulation for efficiency

- Set DAMP_MOD = 10 and load a Hybrid-capable process flow [DS]. The ESPHome enums do not include it [DRV].
- Without a special flow: 1SPW (DAMP_MOD = 01, better above ~15 V) or keep Fsw at 384 kHz [DS, DRV].
- Larger inductance lowers idle current at a given Fsw and PVDD [DS].

### 3.9 Spread spectrum for EMI

- Enable triangle SS in 0x6B and select the profile in 0x6C. The TI example is 0x6C = 0x03 (24 kHz triangle, 25 % range at
  384 kHz) [DS].
- Combine with the default 180-degree L/R phase shift (ANA_CTRL bit 0 = 0) and, for multi-chip builds, PHASE_CTRL offsets [DS].
- Keep the LC filter corner comfortably below the lowest swept frequency.
- Any audible side-effects of SS with a given filter are untested [UNV].

### 3.10 Sample-accurate mute for an OSC-driven synth

- **Soft mute** (DEVICE_CTRL2 bit 3) follows the DIG_VOL_CTRL1 ramp. With one update per sample and 0.5 dB steps, 60 dB takes
  120 samples = 2.5 ms at 48 kHz; with 4 dB steps about 15 samples = 0.3 ms (arithmetic from the register description) [DS].
- **Instant mute**: speed bits = 11 in DIG_VOL_CTRL1 jumps directly, which clicks on tonal content [DS].
- **Hardware MUTEZ**: assign a GPIO as MUTEZ input (GPIO_INPUT_SEL) to force Hi-Z independent of I2C latency [DS].
- True sample accuracy comes from shaping the **I2S stream itself** (envelopes, zeros). I2C writes are millisecond-scale; use the
  chip mutes as safety nets.
- Auto-mute engages after N ms of digital zeros (11.5 ms at 96 kHz, ~23 ms at 48 kHz at the shortest setting) and can clip note
  onsets; set a longer AUTO_MUTE_TIME or clear bits 0-1 of 0x50 for a synth [DS].
- OSC mapping idea: /amp/volume -> DIG_VOL; /amp/gain -> AGAIN; /amp/mute -> DEVICE_CTRL2 bit 3; /amp/eq/n -> BQ n.

### 3.11 Tone generation: frequency response and DC-block high-pass

- Audio bandwidth is set by the DSP rate: 2.0 flows run at 48 or 96 kHz after the SRC [AN]. With 48 kHz I2S, content stops below
  24 kHz, with the filter roll-off near it [UNV exact -3 dB]. Accepting 192 kHz input [DS] does not mean 96 kHz audio bandwidth in a
  96 kHz flow. Ultrasonic output is limited by the DSP rate, the class-D loop bandwidth (80-175 kHz) and the external LC filter [UNV].
- **Low frequency / DC**: the DSP has a DC-block high-pass, default active as far as I can tell (flag default 0 = not bypassed) [AN].
  Cutoff not found [UNV]. Bypassing requires editing DSP parameter memory (PPC3 or manual book/page writes; address [UNV]).
  Even then, sustained DC trips the DC-detect protection [DS].
- Infrasonic or slow control waveforms are therefore a poor fit: expect attenuation and droop on square waves. Use a proper DC-coupled driver.
- Choose AGAIN so that 0 dBFS maps to a safe amplitude for the load; output clips near PVDD regardless.
- Closed-loop gain is supply-independent [DS], so a 1 kHz tone measured across the load is a good calibration check.
- Dynamic range ~109-111 dB [DS] exceeds 16-bit, so 16-bit synthesis from the ESP32 is not the limiting factor.

---

## 4. Practical ESP32 notes (Audio Brick)

- Brick GPIO map (ESP32-S3 variant) [DRV, esparagus-media-center README]: I2C SCL = GPIO9, SDA = GPIO8, PWDN = GPIO17,
  FAULTZ = GPIO18, WARNZ = GPIO04. Confirm against your board revision.
- Bring-up: PWDN high, wait >= 5 ms, start I2S (48 kHz, 64 fs BCLK), I2C: set Hi-Z and clear DIS_DSP, wait 5 ms, load
  biquads / mixer, set Play, then set AGAIN and DIG_VOL [DS].
- Presence check: read DIE_ID (0x67), expect 0x95, at 0x4C [DS].
- Safe first settings with a real load: AGAIN around 0x08-0x10, DIG_VOL around 0x60 (-24 dB), then raise [practical suggestion].
- Defaults 0x00 in DEVICE_CTRL_1 give BTL, BD, 384 kHz. Set bit 2 only when the board is wired for PBTL.
- Keep LRCLK/SCLK running whenever Play is expected; halting them Hi-Zs the outputs and the device recovers by itself [DS].

---

## 5. Sources

- TI product page: https://www.ti.com/product/TAS5825M
- TAS5825M datasheet (SLASEH7H, rev H, Jan 2023): https://www.ti.com/lit/ds/symlink/tas5825m.pdf
- TAS5825M Process Flows app note (SLAA786A): https://www.ti.com/lit/an/slaa786a/slaa786a.pdf
- TAS5825M "Advanced Features" app note: referenced by the datasheet, not read; search ti.com
- mrtoy-me/esphome-tas58xx: https://github.com/mrtoy-me/esphome-tas58xx
- mrtoy-me defs header: https://raw.githubusercontent.com/mrtoy-me/esphome-tas58xx/main/components/tas58xx/tas58xx_defs.h
- sonocotta/esp32-tas5805m-dac: https://github.com/sonocotta/esp32-tas5805m-dac
- sonocotta/esparagus-media-center (Audio Brick): https://github.com/sonocotta/esparagus-media-center

Research limits: GitHub pages were read through a summarising fetch tool; the sonocotta `tas5805m.c` raw URL returned 404 so its
register sequences were not read. TI PDFs were converted locally with pdftotext and read directly; scrambled tables are flagged [UNV].
