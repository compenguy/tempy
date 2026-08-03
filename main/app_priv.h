/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

void bme280_app_init(esp_matter::endpoint_t *temp_sensor_ep, esp_matter::endpoint_t *humidity_sensor_ep);

#if CONFIG_TEMPY_ENABLE_BLE_BEACON
/*
 * Enable / disable the periodic BLE weather beacon.
 *
 * These are gated on CONFIG_TEMPY_ENABLE_BLE_BEACON at the header level so
 * the rest of the app doesn't have to sprinkle #ifdefs at every call site.
 *
 * The beacon owns the BLE controller / NimBLE host. That conflicts with
 * Matter's CHIPoBLE, which uses the same radio and host for commissioning.
 * We therefore leave the beacon OFF at boot and only turn it on once Matter
 * has released BLE (either post-commissioning via kBLEDeinitialized, or on
 * boot when the device is already commissioned so CHIPoBLE never starts).
 * See app_main.cpp:app_event_cb for the state machine.
 *
 * Both calls are idempotent. If bme280_app_init has not run yet, they do
 * nothing.
 */
void bme280_beacon_enable(void);
void bme280_beacon_disable(void);
#endif // CONFIG_TEMPY_ENABLE_BLE_BEACON

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#include "esp_openthread_types.h"

#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs",                                                \
        .netif_queue_size = 10,                                                         \
        .task_queue_size = 10,                                                          \
    }
#endif // CHIP_DEVICE_CONFIG_ENABLE_THREAD
