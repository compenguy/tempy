/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <bsp/esp-bsp.h>
#include <esp_err.h>
#include <esp_log.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <nvs_flash.h>

#include <app_priv.h>
#include <app_reset.h>
#include <common_macros.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static esp_err_t factory_reset_button_register()
{
    button_handle_t push_button;
    esp_err_t err = bsp_iot_button_create(&push_button, nullptr, BSP_BUTTON_NUM);
    VerifyOrReturnError(err == ESP_OK, err);
    return app_reset_button_register(push_button);
}

static void open_commissioning_window_if_necessary()
{
    VerifyOrReturn(chip::Server::GetInstance().GetFabricTable().FabricCount() == 0);

    chip::CommissioningWindowManager & commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    VerifyOrReturn(!commissionMgr.IsCommissioningWindowOpen());

    // After removing last fabric, this example does not remove the Wi-Fi credentials
    // and still has IP connectivity so, only advertising on DNS-SD.
    CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                    chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR)
    {
        ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed successfully");
        open_commissioning_window_if_necessary();
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    // Since this is just a sensor and we don't expect any writes on our temperature sensor,
    // so, return success.
    return ESP_OK;
}

extern "C" void app_main()
{
    /* Initialize the ESP NVS layer */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition truncated, erasing...");
        ESP_ERROR_CHECK_WITHOUT_ABORT(nvs_flash_erase());
        err = nvs_flash_init();
    }

    // usb serial log output seems to be slow to start, so we'll delay a bit
    vTaskDelay(pdMS_TO_TICKS(100));
#if CONFIG_PM_ENABLE
    esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = CONFIG_XTAL_FREQ,  // Allow scaling down to crystal frequency for power savings
#if CONFIG_SLEEP_BETWEEN_READINGS
        .light_sleep_enable = true
#else
        .light_sleep_enable = false
#endif
    };

    ESP_LOGI(TAG, "Initializing power management...");
#if CONFIG_SLEEP_BETWEEN_READINGS
    ESP_LOGW(TAG, "Device uses sleep. Serial debug output will be unreliable.");
#endif
    ESP_ERROR_CHECK_WITHOUT_ABORT(esp_pm_configure(&pm_config));
#endif

    /* Initialize push button on the dev-kit to reset the device */
    ESP_LOGI(TAG, "Initializing reset button...");
    ESP_ERROR_CHECK_WITHOUT_ABORT(factory_reset_button_register());

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = nullptr;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Creating Matter node...");
        node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
        if (node == nullptr) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create Matter node");
        }
    }

    // add temperature sensor device
    temperature_sensor::config_t temp_sensor_config;
    endpoint_t *temp_sensor_ep = nullptr;
    if (err == ESP_OK && node != nullptr) {
        ESP_LOGI(TAG, "Registering temperature sensor Matter endpoint...");
        temp_sensor_ep = temperature_sensor::create(node, &temp_sensor_config, ENDPOINT_FLAG_NONE, nullptr);
        if (temp_sensor_ep == nullptr) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create temperature sensor endpoint");
        }
    }

    // add the humidity sensor device
    humidity_sensor::config_t humidity_sensor_config;
    endpoint_t *humidity_sensor_ep = nullptr;
    if (err == ESP_OK && node != nullptr) {
        ESP_LOGI(TAG, "Registering humidity sensor Matter endpoint...");
        humidity_sensor_ep = humidity_sensor::create(node, &humidity_sensor_config, ENDPOINT_FLAG_NONE, nullptr);
        if (humidity_sensor_ep == nullptr) {
            err = ESP_FAIL;
            ESP_LOGE(TAG, "Failed to create humidity sensor endpoint");
        }
    }

    ESP_LOGI(TAG, "Starting bme280 temperature/humidity sensor...");
    bme280_app_init(temp_sensor_ep, humidity_sensor_ep);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config.
     * set_openthread_platform_config stores the pointer, so this must outlive
     * the OpenThread stack -- static storage is required. */
    ESP_LOGI(TAG, "Starting Thread networking...");
    static esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif

    /* Matter start */
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Starting Matter framework...");
        err = esp_matter::start(app_event_cb);
        ESP_ERROR_CHECK_WITHOUT_ABORT(err);
    }
}
