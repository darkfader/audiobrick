# Connectors, free pins and what they could be used for (Audio Brick, ESP32 variant)

Source: the rev **C1** schematic (`hardware/5-esparagus-audio-brick/rev-c1/2603-esparagus-audio-brick-c1-schematic.pdf`, 3 pages, ESP32-WROVER-IE 16 MB), the C1 BOM and
PCB PDF, the ESPHome YAML and the shop pages from <https://github.com/sonocotta/esparagus-media-center>, read by a research pass on 2026-10-04.
C1 is the newest single-DAC ESP32 revision in that repository; **the owner's board has no revision print, so C1 is an assumption**. The nets in the schematic are
drawn, not netlisted: where a statement says *inferred* it is read from a net flag or a cross mark. Nothing here has been checked on the physical board yet.

## Connectors and headers

| Ref | What | Signals | Level | Stock use |
|---|---|---|---|---|
| CN1 | JST SH 1.0 mm 4-pin, labelled **QWIIC** | 1 GND, 2 3V3, 3 SDA = GPIO21, 4 SCL = GPIO27 | 3.3 V | nothing; it is the same I2C bus as the TAS5825M (0x4C) |
| CN2 (+ plug P1) | 4-pin 5.08 mm terminal | amp outputs OUT_A+/-, OUT_B+/-; solder jumpers SJ5/SJ6 select PBTL (mono bridging) | amp output | speakers |
| CN4 | 2-pin 5.08 mm terminal, in parallel with barrel jack DC2 | VDD 5-26 V, GND; D3 (M7) across the rail | VDD | power |
| CN5 | 2-pin 5.08 mm terminal | drawn with NC crosses | none | nothing |
| RJ2 | RJ45 with magnetics | W5500 PHY, link/activity LEDs on the jack | 3.3 V | Ethernet |
| USB2 | USB-C | VBUS -> D2/D4 -> VDD; D+/D- -> CH340C -> UART0 (GPIO1/3) with DTR/RTS auto-reset | 5 V | flashing, logs |
| FPC3 | 30-pin 0.5 mm FPC, "30Pin OLED" | pin 13 CS = GPIO15, 14 RES = GPIO32, 15 DC = GPIO4, 18 SCLK = GPIO18, 19 MOSI = GPIO23; 6 and 9 = 3V3; 1, 8, 10-12, 29, 30 = GND; 2-5 and 26-28 carry panel capacitors/resistors; **no MISO** | 3.3 V | optional OLED |
| SW3 / SW4 | tact switches | EN (reset) / GPIO0 (BOOT, usable as a button after boot) | | |
| LED5 | WS2812B | GPIO12 via 100 ohm | 3.3 V | status |
| FAULT1, WARN1 | discrete LEDs | driven from the amp's FAULTZ and WARNZ nets | | |
| "MIC HDR (NOT INSTALLED)" | schematic block for an I2S MEMS microphone (ICS-43434 style) | I2S clock = GPIO26 and word select = GPIO25 (shared with the DAC), data in *appears* to be GPIO13 | 3.3 V | not fitted; no connector symbol or pad seen |

**Power connector discrepancy:** the C1 schematic and BOM show a 2-pin CN4 plus the barrel jack, but the shop pages talk about a "4-pin snap-in power connector" and the C1 PCB
note mentions a CN3 that is not in the schematic. So the shipped board is probably newer or different from C1; check the physical board (and polarity) before connecting a supply.

## GPIOs

Used: 0 (BOOT), 1/3 (UART0, only to the CH340), 4 (OLED DC), 5 (W5500 CS, strapping), 12 (WS2812, strapping), 14 (W5500 RST), 15 (OLED CS, strapping),
18/19/23 (SPI3, shared by W5500 and the OLED), 21/27 (I2C), 22/25/26 (I2S), 32 (OLED RES), 33 (DAC PWDN), 35 (W5500 INT), 36 (WARNZ), 39 (FAULTZ).
Not available: 6-11 (flash), 16/17 (PSRAM on the WROVER).

Free: **GPIO34** (input only, marked NC), **GPIO2** (strapping pin, must be low or floating at boot, marked NC), **GPIO13** (inferred: only the unfitted mic net uses it).
With no OLED fitted, GPIO4, GPIO32 and GPIO15 (strapping) are free too. **None of these is brought to a header**: using them means a bodge wire to the module pad.
The only broken-out interfaces are CN1 (I2C), FPC3 (SPI plus CS/DC/RES), USB (UART via the CH340) and SW4.

## What you could do with them

| Idea | How | Notes and conflicts |
|---|---|---|
| I2C sensors (temperature, humidity, distance, IMU, RTC) | plug into **CN1** (Qwiic) | same bus as the amp: avoid address 0x4C, add a bus lock around amp register writes; the schematic shows no pull-ups on sheet 1 (unknown whether they exist, the amp scan works) |
| I2C display | CN1 | needs a driver; fine at 400 kHz for a short cable |
| SPI OLED / small TFT | **FPC3** (30-pin 0.5 mm FPC, write-only SPI) | shares SPI3 with the W5500: separate CS per device and a bus lock; heavy Ethernet traffic during display refresh is the risk. A different panel needs its own pinout |
| I2S microphone (room measurement, level, voice) | bodge wires to 26/25 plus a data-in pin (probably GPIO13) | full duplex on one I2S port: TX on GPIO22, RX on the data pin, same clocks; the stock mic table in the upstream README lists only S3 pins |
| Rotary encoder or buttons | GPIO34 (needs an external pull-up), GPIO2, GPIO13; SW4 (GPIO0) as a button after boot | the upstream rotary-encoder table is for another board and conflicts (27, 33, 34) |
| IR receiver | GPIO34, GPIO13 or GPIO2 | do **not** use GPIO39 (that is FAULTZ on this board, although the Louder board uses it for IR) |
| Extra WS2812 strip | GPIO13, GPIO2 (or GPIO4 without OLED) via RMT | level-shift for long strips and power the strip from VDD, not 3V3 |
| Second UART | any free pin through the GPIO matrix | UART0 is the log/flash port |
| SD card | not practical | no slot; needs about four free pins that do not exist without collisions (GPIO12/2/13/15) |
| Line-in / second I2S output | not on the board | internal ADC on GPIO34 is noisy; a second I2S port on free pins needs bodge wires |
| Mono bridged output | solder jumpers SJ5/SJ6 (PBTL) | changes the amp mode in firmware too (BTL -> PBTL); do not do it with stereo speakers connected |

## Confirmed versus inferred

- **Confirmed from the C1 schematic:** the GPIO assignments above for I2S, I2C, SPI, W5500, LED, OLED pins; CN1 as a 4-pin SH connector on GPIO21/27 with 3V3 and GND; the FPC3 pin map; CN4/CN5/DC2 power parts; SW3/SW4; the "NOT INSTALLED" mic block.
- **Inferred:** mic data on GPIO13 (net flag only); GPIO2 and GPIO34 unconnected (cross marks); the OLED panel type (SSD1306/SH1106 according to the shop pages).
- **Unknown:** the owner's board revision and connector set (2-pin vs 4-pin power, order of the 4 pins); whether I2C pull-ups exist; the physical pin-1 marking of CN1.

## How to check on the real board

1. Look near the DC jack for a revision print or silkscreen, and count the pins of the power terminal.
2. Check whether a JST SH 4-pin socket (CN1) and an FPC socket are fitted (the FPC fits a 30-pin 0.5 mm ribbon).
3. Before connecting anything to CN1, measure 3V3 and GND on its pins; beep from the SDA/SCL pins to GPIO21/27 on the module (or the amp's I2C pads).
4. Expect GPIO13, GPIO2 and GPIO34 to be open (not connected to anything) on the module pads.
