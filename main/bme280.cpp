#include <string.h> // required by memset
#include <esp_log.h>
#include <esp_err.h>
#include <esp_check.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include "bme280.h"

static const char *TAG = "BME280";

// --------- BME280 registers ----------
#define REG_ID          0xD0
#define REG_RESET       0xE0
#define REG_CTRL_HUM    0xF2
#define REG_STATUS      0xF3
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7  // 0xF7..0xFE data block

#define REG_CALIB00     0x88  // 0x88..0xA1
#define REG_CALIB26     0xE1  // 0xE1..0xE7

#define RESET_CMD       0xB6
#define CHIP_ID_BME280  0x60

// ctrl_meas bits
#define MODE_SLEEP   0x00
#define MODE_FORCED  0x01
#define MODE_NORMAL  0x03

// oversampling
#define OSRS_SKIP 0x00
#define OSRS_1X   0x01
#define OSRS_2X   0x02
#define OSRS_4X   0x03
#define OSRS_8X   0x04
#define OSRS_16X  0x05

// --------- I2C helpers ----------
static esp_err_t bme280_write(bme280_t *bme, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(bme->dev, buf, sizeof(buf), -1);
}

static esp_err_t bme280_read(bme280_t *bme, uint8_t reg, uint8_t *data, size_t len)
{
    esp_err_t err = i2c_master_transmit(bme->dev, &reg, 1, -1);
    if (err != ESP_OK) return err;
    return i2c_master_receive(bme->dev, data, len, -1);
}

static esp_err_t bme280_read_u8(bme280_t *bme, uint8_t reg, uint8_t *val)
{
    return bme280_read(bme, reg, val, 1);
}

// --------- Calibration parsing ----------
static void parse_calib(bme280_calib_t *c, const uint8_t calib1[26], const uint8_t calib2[7])
{
    c->dig_T1 = (uint16_t)(calib1[1] << 8 | calib1[0]);
    c->dig_T2 = (int16_t)(calib1[3] << 8 | calib1[2]);
    c->dig_T3 = (int16_t)(calib1[5] << 8 | calib1[4]);

    c->dig_P1 = (uint16_t)(calib1[7] << 8 | calib1[6]);
    c->dig_P2 = (int16_t)(calib1[9] << 8 | calib1[8]);
    c->dig_P3 = (int16_t)(calib1[11] << 8 | calib1[10]);
    c->dig_P4 = (int16_t)(calib1[13] << 8 | calib1[12]);
    c->dig_P5 = (int16_t)(calib1[15] << 8 | calib1[14]);
    c->dig_P6 = (int16_t)(calib1[17] << 8 | calib1[16]);
    c->dig_P7 = (int16_t)(calib1[19] << 8 | calib1[18]);
    c->dig_P8 = (int16_t)(calib1[21] << 8 | calib1[20]);
    c->dig_P9 = (int16_t)(calib1[23] << 8 | calib1[22]);

    c->dig_H1 = calib1[25];
    c->dig_H2 = (int16_t)(calib2[1] << 8 | calib2[0]);
    c->dig_H3 = calib2[2];

    // H4 and H5 are packed across bytes
    c->dig_H4 = (int16_t)((calib2[3] << 4) | (calib2[4] & 0x0F));
    c->dig_H5 = (int16_t)((calib2[5] << 4) | (calib2[4] >> 4));
    // 12-bit sign extension for H4 and H5
    if (c->dig_H4 & 0x0800) { c->dig_H4 |= 0xF000; }
    if (c->dig_H5 & 0x0800) { c->dig_H5 |= 0xF000; }

    c->dig_H6 = (int8_t)calib2[6];
}

// --------- Compensation (datasheet) ----------
static int32_t compensate_temp_centiC(bme280_calib_t *c, int32_t adc_T)
{
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)c->dig_T1 << 1))) * ((int32_t)c->dig_T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)c->dig_T1)) *
                      ((adc_T >> 4) - ((int32_t)c->dig_T1))) >> 12) *
                    ((int32_t)c->dig_T3)) >> 14;
    c->t_fine = var1 + var2;
    int32_t T = (c->t_fine * 5 + 128) >> 8; // centi-degC
    return T;
}

static uint32_t compensate_hum_permille(bme280_calib_t *c, int32_t adc_H)
{
    int64_t v_x1;

    // v_x1 = t_fine - 76800
    v_x1 = (int64_t)c->t_fine - 76800;

    // First big bracket:
    // (((adc_H << 14) - (dig_H4 << 20) - (dig_H5 * v_x1) + 16384) >> 15)
    int64_t a = ((int64_t)adc_H << 14)
              - ((int64_t)c->dig_H4 << 20)
              - ((int64_t)c->dig_H5 * v_x1);
    a = (a + 16384) >> 15;

    // Second big bracket:
    // (((((((v_x1 * dig_H6) >> 10) * (((v_x1 * dig_H3) >> 11) + 32768)) >> 10)
    //     + 2097152) * dig_H2 + 8192) >> 14)
    int64_t b = (v_x1 * (int64_t)c->dig_H6) >> 10;
    b = (b * (((v_x1 * (int64_t)c->dig_H3) >> 11) + 32768)) >> 10;
    b = (b + 2097152);
    b = (b * (int64_t)c->dig_H2 + 8192) >> 14;

    // Multiply brackets
    v_x1 = a * b;

    // Subtract nonlinearity term:
    // v_x1 - (((v_x1 >> 15)^2 >> 7) * dig_H1 >> 4)
    int64_t v_sq = (v_x1 >> 15);
    v_sq = (v_sq * v_sq) >> 7;
    v_x1 = v_x1 - ((v_sq * (int64_t)c->dig_H1) >> 4);

    // Clamp per Bosch
    if (v_x1 < 0) v_x1 = 0;
    if (v_x1 > 419430400) v_x1 = 419430400;

    // Convert to permille
    uint32_t H_q22_10 = (uint32_t)(v_x1 >> 12); // %RH * 1024

    // permille means %RH * 10 (0..1000)
    // so permille = (H / 1024) * 10 = H * 10 / 1024
    uint32_t permille = (H_q22_10 * 10) / 1024;

    if (permille > 1000) permille = 1000;
    return permille;
}

// --------- Public API ----------
esp_err_t bme280_init(bme280_t *bme,
                      i2c_master_bus_handle_t bus,
                      uint8_t i2c_addr,
                      uint32_t i2c_freq)
{
    memset(bme, 0, sizeof(*bme));
    bme->bus = bus;
    bme->addr = i2c_addr;

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_addr,
        .scl_speed_hz = i2c_freq,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus, &dev_cfg, &bme->dev), TAG, "add dev");

    // Check chip ID
    uint8_t id = 0;
    ESP_RETURN_ON_ERROR(bme280_read_u8(bme, REG_ID, &id), TAG, "read id");
    if (id != CHIP_ID_BME280) {
        ESP_LOGE(TAG, "Unexpected chip ID 0x%02X", id);
        return ESP_ERR_NOT_FOUND;
    }

    // Soft reset
    ESP_RETURN_ON_ERROR(bme280_write(bme, REG_RESET, RESET_CMD), TAG, "reset");
    vTaskDelay(pdMS_TO_TICKS(5));

    // Wait for NVM copy to finish
    uint8_t status;
    do {
        ESP_RETURN_ON_ERROR(bme280_read_u8(bme, REG_STATUS, &status), TAG, "status");
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (status & 0x01);

    // Read calibration blocks
    uint8_t calib1[26], calib2[7];
    ESP_RETURN_ON_ERROR(bme280_read(bme, REG_CALIB00, calib1, sizeof(calib1)), TAG, "calib1");
    ESP_RETURN_ON_ERROR(bme280_read(bme, REG_CALIB26, calib2, sizeof(calib2)), TAG, "calib2");
    parse_calib(&bme->calib, calib1, calib2);

    // Humidity oversampling must be written before ctrl_meas
    ESP_RETURN_ON_ERROR(bme280_write(bme, REG_CTRL_HUM, OSRS_1X), TAG, "ctrl_hum");

    // Standby/filter config (doesn't matter in forced mode much)
    ESP_RETURN_ON_ERROR(bme280_write(bme, REG_CONFIG, 0x00), TAG, "config");

    // Temp oversampling 1x, Press oversampling 1x (ignored), sleep mode for now
    uint8_t ctrl_meas = (OSRS_1X << 5) | (OSRS_1X << 2) | MODE_SLEEP;
    ESP_RETURN_ON_ERROR(bme280_write(bme, REG_CTRL_MEAS, ctrl_meas), TAG, "ctrl_meas");

    ESP_LOGI(TAG, "BME280 init OK @0x%02X", i2c_addr);
    return ESP_OK;
}

esp_err_t bme280_read_temp_humidity(bme280_t *bme, float *temp_c, float *rh)
{
    // Trigger forced measurement:
    uint8_t ctrl_meas = (OSRS_1X << 5) | (OSRS_1X << 2) | MODE_FORCED;
    ESP_RETURN_ON_ERROR(bme280_write(bme, REG_CTRL_MEAS, ctrl_meas), TAG, "start forced");

    // Wait until measuring bit clears
    uint8_t status;
    do {
        ESP_RETURN_ON_ERROR(bme280_read_u8(bme, REG_STATUS, &status), TAG, "status");
        vTaskDelay(pdMS_TO_TICKS(2));
    } while (status & 0x08);

    // Read data burst (press[3], temp[3], hum[2]) = 8 bytes
    uint8_t data[8];
    ESP_RETURN_ON_ERROR(bme280_read(bme, REG_PRESS_MSB, data, sizeof(data)), TAG, "read data");

    int32_t adc_T = (int32_t)((data[3] << 12) | (data[4] << 4) | (data[5] >> 4));
    int32_t adc_H = (int32_t)((data[6] << 8) | data[7]);

    int32_t t_centi = compensate_temp_centiC(&bme->calib, adc_T);
    uint32_t h_permille = compensate_hum_permille(&bme->calib, adc_H);

    if (temp_c) *temp_c = t_centi / 100.0f;
    if (rh)     *rh     = h_permille / 10.0f; // permille -> %

    return ESP_OK;
}
