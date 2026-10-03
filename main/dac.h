#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

// Bring up the TAS5825M. I2S BCK/WS must already be running (the part has no MCLK input).
// The amp is left in PLAY but muted, at DAC_DEFAULT_VOLUME_DB.
#define DAC_DEFAULT_VOLUME_DB (-38)  // gentle start; raise it from the web page

bool dac_init(void);

// Idle power saving: muted for hiz_s -> outputs Hi-Z; muted for off_s -> chip powered down. Any unmute wakes it.
typedef enum { AMP_ACTIVE, AMP_HIZ, AMP_OFF } amp_state_t;
amp_state_t dac_state(void);
bool dac_power_set(int hiz_s, int off_s);       // seconds, 0 = never; saved in flash
void dac_power_get(int *hiz_s, int *off_s);
void dac_http_register(httpd_handle_t server);  // GET/POST /power
bool dac_set_mute(bool mute);
bool dac_set_volume_db(int db);  // clamped to [-90, speaker-profile cap]
int dac_get_volume_db(void);
bool dac_read_reg(uint8_t reg, uint8_t *val);  // book 0, page 0 registers
float dac_pvdd_volts(void);      // from the chip's PVDD ADC (0x5E); negative if unreadable
bool dac_clear_faults(void);     // clear latched analog faults (FAULT_CLEAR, 0x78 bit 7)
bool dac_fault_active(void);     // FAULTZ low
bool dac_warning_active(void);   // WARNZ low
