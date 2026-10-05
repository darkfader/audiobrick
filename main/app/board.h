// Esparagus Audio Brick, ESP32 revision. The ESP32-S3 revision uses different pins.
#pragma once

#define PIN_I2S_BCK   26
#define PIN_I2S_WS    25
#define PIN_I2S_DOUT  22

#define PIN_I2C_SDA   21
#define PIN_I2C_SCL   27
#define DAC_I2C_ADDR  0x4C

#define PIN_DAC_PWDN  33
#define PIN_DAC_FAULT 39  // FAULTZ, active low, input only
#define PIN_DAC_WARN  36  // WARNZ, active low, input only

#define PIN_LED       12  // WS2812

#define PIN_SPI_SCLK  18
#define PIN_SPI_MOSI  23
#define PIN_SPI_MISO  19
#define PIN_ETH_CS    5
#define PIN_ETH_INT   35
#define PIN_ETH_RST   14

#define SAMPLE_RATE   48000
