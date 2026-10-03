// TAS5825M control. The power-up register sequence follows the field-tested one from
// github.com/mrtoy-me/esphome-tas58xx (via rmalchow/ondaire tas58xx.c). Registers marked
// "vendor" have no datasheet meaning there and are copied as-is.
#include "dac.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "speaker_limits.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "dac";

#define REG_PAGE      0x00
#define REG_RESET     0x01
#define REG_DEV_CTRL1 0x02  // BTL/PBTL, modulation
#define REG_DEV_CTRL2 0x03  // CTRL_STATE: 0 deep sleep, 2 Hi-Z, 3 play; bit 3 = mute
#define REG_DIG_VOL   0x4C  // 0x30 = 0 dB, -0.5 dB per LSB, 0xFF = mute
#define REG_AGAIN     0x54  // analog gain, 0x00 = 0 dB
#define REG_FAULT_CLR 0x78
#define REG_BOOK      0x7F

#define CTRL2_PLAY    0x03
#define CTRL2_MUTE    0x08
#define DELAY_MARK    0xFE  // pseudo-register: value is a delay in ms

typedef struct { uint8_t reg, val; } regval_t;

static const regval_t INIT_SEQ[] = {
    { REG_PAGE,      0x00 },
    { REG_BOOK,      0x00 },
    { REG_DEV_CTRL2, 0x02 },  // Hi-Z
    { REG_RESET,     0x11 },  // reset core and registers
    { REG_DEV_CTRL2, 0x02 },
    { DELAY_MARK,    0x05 },
    { REG_DEV_CTRL2, 0x00 },  // deep sleep
    { 0x46,          0x11 },  // vendor: clock/sample-rate config (48 kHz)
    { REG_DEV_CTRL2, 0x02 },
    { 0x61,          0x0B },  // vendor
    { 0x60,          0x01 },  // vendor
    { 0x7D,          0x11 },  // vendor
    { 0x7E,          0xFF },  // vendor
    { REG_PAGE,      0x01 },
    { 0x51,          0x05 },  // vendor (page 1)
    { REG_PAGE,      0x00 },
    { REG_BOOK,      0x00 },
    { REG_DEV_CTRL1, 0x00 },  // BTL, normal modulation
    { 0x30,          0x00 },  // vendor
    { REG_DIG_VOL,   0x30 + (-DAC_DEFAULT_VOLUME_DB) * 2 },
    { 0x53,          0x00 },  // vendor
    { REG_AGAIN,     0x00 },
    { REG_DEV_CTRL2, CTRL2_PLAY | CTRL2_MUTE },  // play, but muted
    { REG_FAULT_CLR, 0x80 },
};

static i2c_master_dev_handle_t s_dev;
static int s_vol_db = DAC_DEFAULT_VOLUME_DB;

static bool write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    esp_err_t err = i2c_master_transmit(s_dev, buf, sizeof buf, 50);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "write 0x%02X=0x%02X failed: %s", reg, val, esp_err_to_name(err));
    }
    return err == ESP_OK;
}

bool dac_init(void)
{
    gpio_config_t pwdn = {
        .pin_bit_mask = 1ULL << PIN_DAC_PWDN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&pwdn);
    // TI power-up timing: low, >=1 ms, high, >=5 ms before I2C.
    gpio_set_level(PIN_DAC_PWDN, 0);
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(PIN_DAC_PWDN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    // GPIO36/39 are input only with no internal pulls; the board pulls them up.
    gpio_config_t status = {
        .pin_bit_mask = (1ULL << PIN_DAC_FAULT) | (1ULL << PIN_DAC_WARN),
        .mode = GPIO_MODE_INPUT,
    };
    gpio_config(&status);

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed");
        return false;
    }
    if (i2c_master_probe(bus, DAC_I2C_ADDR, 50) != ESP_OK) {
        ESP_LOGE(TAG, "no ACK from TAS5825M at 0x%02X", DAC_I2C_ADDR);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DAC_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "I2C add device failed");
        return false;
    }

    for (size_t i = 0; i < sizeof INIT_SEQ / sizeof INIT_SEQ[0]; i++) {
        if (INIT_SEQ[i].reg == DELAY_MARK) {
            vTaskDelay(pdMS_TO_TICKS(INIT_SEQ[i].val));
        } else if (!write_reg(INIT_SEQ[i].reg, INIT_SEQ[i].val)) {
            return false;
        }
    }
    dac_set_volume_db(DAC_DEFAULT_VOLUME_DB);  // applies the profile's volume cap
    ESP_LOGI(TAG, "TAS5825M ready: BTL, %d dB (cap %d dB), muted", s_vol_db, limits_max_volume_db());
    return true;
}

bool dac_set_mute(bool mute)
{
    return write_reg(REG_DEV_CTRL2, CTRL2_PLAY | (mute ? CTRL2_MUTE : 0));
}

bool dac_set_volume_db(int db)
{
    // The speaker profile caps the volume so even a 0 dBFS signal stays under the SPL limit.
    int cap = limits_max_volume_db();
    if (db > cap) db = cap;
    if (db < -90) db = -90;
    s_vol_db = db;
    return write_reg(REG_DIG_VOL, (uint8_t)(0x30 + (-db) * 2));
}

int dac_get_volume_db(void)
{
    return s_vol_db;
}

bool dac_read_reg(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, 50) == ESP_OK;
}

float dac_pvdd_volts(void)
{
    uint8_t raw;
    if (!dac_read_reg(0x5E, &raw)) return -1.0f;
    return raw / 8.428f;  // datasheet: 223 = 26.45 V, 38 = 4.51 V
}

bool dac_clear_faults(void)
{
    return write_reg(REG_FAULT_CLR, 0x80);
}

bool dac_fault_active(void)   { return gpio_get_level(PIN_DAC_FAULT) == 0; }
bool dac_warning_active(void) { return gpio_get_level(PIN_DAC_WARN) == 0; }
