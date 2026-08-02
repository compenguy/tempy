#ifndef COMMON_H
#define COMMON_H

/* Includes */
/* STD APIs */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* ESP APIs */
#include <esp_log.h>
#include <sdkconfig.h>

/* FreeRTOS APIs */
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/* NimBLE stack APIs */
#include <host/ble_hs.h>
#include <host/ble_uuid.h>
#include <host/util/util.h>
#include <nimble/ble.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Defines */
#define DEVICE_NAME "Tempy"

/* BLE Appearance Values */
#define BLE_APPEARANCE_GENERIC_THERMOMETER 0x0300

/* Sensor data structure */
typedef struct {
    float temperature; // degrees C, use NAN for no reading
    float humidity; // percent, use NAN for no reading
    float pressure; // hPa, use NAN for no reading
} weather_data_t;

#ifdef __cplusplus
}
#endif

#endif // COMMON_H
