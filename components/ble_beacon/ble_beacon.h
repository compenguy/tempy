#ifndef BLE_BEACON_H
#define BLE_BEACON_H

#include <stdint.h>
#include <esp_err.h>
#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the BLE beacon
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_beacon_init(void);

/**
 * @brief Start BLE advertising with sensor data
 * @param data Sensor data to advertise
 * @param duration_ms Duration of advertising in milliseconds
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_beacon_start(weather_data_t *data, uint32_t duration_ms);

/**
 * @brief Stop BLE advertising
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_beacon_stop(void);

/**
 * @brief Deinitialize the BLE beacon
 * @return ESP_OK on success, error code otherwise
 */
esp_err_t ble_beacon_deinit(void);

#ifdef __cplusplus
}
#endif

#endif // BLE_BEACON_H
