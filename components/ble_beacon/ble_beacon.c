#include "ble_beacon.h"
#include "common.h"
#include "gap.h"

/* Library function declarations */
void ble_store_config_init(void);

/* Private function declarations */
static void on_stack_reset(int reason);
static void on_stack_sync(void);
static void nimble_host_config_init(void);
static void nimble_host_task(void *param);

/* Private variables */
static bool initialized = false;
static bool synced = false;
static TaskHandle_t host_task_handle = NULL;
static uint32_t adv_duration_ms = 0;
static SemaphoreHandle_t adv_semaphore = NULL;

/* Stack event callback functions */
static void on_stack_reset(int reason) {
    ESP_LOGI(TAG, "NimBLE stack reset, reset reason: %d", reason);
    synced = false;
}

static void on_stack_sync(void) {
    ESP_LOGI(TAG, "NimBLE stack synchronized");
    synced = true;
    adv_init();
}

static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Store host configuration */
    ble_store_config_init();
}

static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "NimBLE host task started");

    /* This function won't return until nimble_port_stop() is executed */
    nimble_port_run();

    ESP_LOGI(TAG, "NimBLE host task ended");
    vTaskDelete(NULL);
}

static void adv_timer_task(void *param) {
    /* Wait for advertising duration */
    vTaskDelay(pdMS_TO_TICKS(adv_duration_ms));

    /* Stop advertising only if we're still running */
    if (initialized) {
        adv_stop();
    }

    /* Signal completion */
    if (adv_semaphore != NULL) {
        xSemaphoreGive(adv_semaphore);
    }

    vTaskDelete(NULL);
}

esp_err_t ble_beacon_init(void) {
    ESP_LOGI(TAG, "Initializing BLE beacon");

    if (initialized) {
        ESP_LOGW(TAG, "BLE beacon already initialized");
        return ESP_OK;
    }

    /* Create semaphore for advertising duration control */
    adv_semaphore = xSemaphoreCreateBinary();
    if (adv_semaphore == NULL) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return ESP_FAIL;
    }

    /* Initialize NimBLE */
    int rc = nimble_port_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initialize NimBLE port, error code: %d", rc);
        vSemaphoreDelete(adv_semaphore);
        adv_semaphore = NULL;
        return ESP_FAIL;
    }

    /* Initialize GAP service */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "Failed to initialize GAP service, error code: %d", rc);
        nimble_port_deinit();
        vSemaphoreDelete(adv_semaphore);
        adv_semaphore = NULL;
        return ESP_FAIL;
    }

    /* Initialize NimBLE host configuration */
    nimble_host_config_init();

    /* Start NimBLE host task */
    rc = xTaskCreate(nimble_host_task, "NimBLE Host", 4096, NULL, 5, &host_task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create NimBLE host task");
        nimble_port_deinit();
        vSemaphoreDelete(adv_semaphore);
        adv_semaphore = NULL;
        return ESP_FAIL;
    }

    /* Wait for stack to sync */
    for (int i = 0; i < 10 && !synced; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (!synced) {
        ESP_LOGE(TAG, "Timeout waiting for NimBLE stack to sync");
        // Don't try to stop the task or nimble_port here - it might cause issues
        // Just report the error and let the caller handle it
        return ESP_FAIL;
    }

    initialized = true;
    return ESP_OK;
}

esp_err_t ble_beacon_start(weather_data_t *data, uint32_t duration_ms) {
    if (!initialized) {
        ESP_LOGE(TAG, "BLE beacon not initialized");
        return ESP_FAIL;
    }

    if (data == NULL) {
        ESP_LOGE(TAG, "Invalid sensor data");
        return ESP_ERR_INVALID_ARG;
    }

    if (!synced) {
        ESP_LOGE(TAG, "BLE stack not synchronized");
        return ESP_FAIL;
    }

    /* Update sensor data in advertisement */
    adv_update_sensor_data(data);

    /* Store advertising duration */
    adv_duration_ms = duration_ms;

    /* Reset semaphore */
    xSemaphoreTake(adv_semaphore, 0);

    /* Create timer task to stop advertising after duration */
    TaskHandle_t timer_task_handle = NULL;
    BaseType_t rc = xTaskCreate(adv_timer_task, "ADV Timer", 2048, NULL, 3, &timer_task_handle);
    if (rc != pdPASS) {
        ESP_LOGE(TAG, "Failed to create advertising timer task");
        adv_stop(); // Stop advertising since we can't control its duration
        return ESP_FAIL;
    }

    /* Wait for advertising to complete */
    if (xSemaphoreTake(adv_semaphore, pdMS_TO_TICKS(duration_ms + 2000)) != pdTRUE) {
        ESP_LOGW(TAG, "Advertising timeout - continuing anyway");
        adv_stop();
        return ESP_OK; // Return OK since this isn't a critical failure
    }

    return ESP_OK;
}

esp_err_t ble_beacon_stop(void) {
    if (!initialized) {
        return ESP_OK;
    }

    /* Stop advertising */
    adv_stop();

    return ESP_OK;
}

esp_err_t ble_beacon_deinit(void) {
    ESP_LOGI(TAG, "Deinitializing BLE beacon");

    if (!initialized) {
        return ESP_OK;
    }

    /* Clear initialized flag first to prevent other operations */
    initialized = false;

    /* Stop advertising */
    adv_stop();

    /* Attempt to stop NimBLE host task (may fail if already stopping) */
    if (synced) {
        nimble_port_stop();

        /* Give time for the host task to clean up */
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    /* Delete semaphore */
    if (adv_semaphore != NULL) {
        vSemaphoreDelete(adv_semaphore);
        adv_semaphore = NULL;
    }

    /* Try to deinitialize NimBLE port */
    nimble_port_deinit();

    synced = false;
    return ESP_OK;
}
