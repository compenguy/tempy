#pragma once

#include <esp_err.h>
#include <driver/i2c_master.h>

typedef struct {
    // Temperature calib
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    // Pressure calib (not used for output but needed to parse block)
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    // Humidity calib
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;

    int32_t  t_fine;
} bme280_calib_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t addr;
    bme280_calib_t calib;
} bme280_t;

esp_err_t bme280_init(bme280_t *bme, i2c_master_bus_handle_t bus, uint8_t i2c_addr, uint32_t i2c_freq);
esp_err_t bme280_read_temp_humidity(bme280_t *bme, float *temp_c, float *rh);
