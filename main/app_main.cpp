/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <bsp/esp-bsp.h>
#include <button_gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#if CONFIG_PM_ENABLE
#include <esp_pm.h>
#endif
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <nvs_flash.h>
#include <setup_payload/OnboardingCodesUtil.h>

#include <app_priv.h>
#include <app_reset.h>
#include <common_macros.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

// Enrich the root node's Basic Information cluster with the optional attributes
// (ProductLabel, PartNumber, ProductURL, SerialNumber, ManufacturingDate) so
// commissioners like Home Assistant can display them. root_node::create only
// installs the mandatory subset; anything else has to be added by hand here.
// The values passed at create time are placeholders -- these attributes are
// marked ATTRIBUTE_FLAG_MANAGED_INTERNALLY, which routes reads through
// GetDeviceInstanceInfoProvider() and ultimately the `fctry` NVS partition
// baked by esp-matter-mfg-tool. Also add ThreadNetworkDiagnostics on the same
// endpoint since we're a Thread device; that gives fabric-side tools a
// well-defined place to read the actual network name / PAN ID rather than
// guessing from other identifiers.
static esp_err_t enrich_root_node_clusters(node_t *node)
{
    endpoint_t *root_ep = endpoint::get(node, 0);
    VerifyOrReturnError(root_ep != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Root endpoint not found"));

    cluster_t *basic_info = cluster::get(root_ep, BasicInformation::Id);
    VerifyOrReturnError(basic_info != nullptr, ESP_FAIL,
                        ESP_LOGE(TAG, "Basic Information cluster not found on root endpoint"));

    cluster::basic_information::attribute::create_product_label(basic_info, nullptr, 0);
    cluster::basic_information::attribute::create_part_number(basic_info, nullptr, 0);
    cluster::basic_information::attribute::create_product_url(basic_info, nullptr, 0);
    cluster::basic_information::attribute::create_serial_number(basic_info, nullptr, 0);
    cluster::basic_information::attribute::create_manufacturing_date(basic_info, nullptr, 0);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    cluster::thread_network_diagnostics::config_t td_config;
    VerifyOrReturnError(cluster::thread_network_diagnostics::create(root_ep, &td_config, CLUSTER_FLAG_SERVER) != nullptr,
                        ESP_FAIL, ESP_LOGE(TAG, "Failed to create ThreadNetworkDiagnostics cluster"));
#endif
    return ESP_OK;
}

static esp_err_t factory_reset_button_register()
{
    // Bypass bsp_iot_button_create() so we can set enable_power_save when
    // we're actually going to light-sleep. That flag arms the GPIO as a
    // light-sleep wake source and only runs the button-scan timer while
    // the button is being held; the BSP's static bsp_button_config[]
    // leaves it at its default (false), which would drop press events
    // while the CPU is asleep and also keep the periodic scan timer
    // running the whole time.
    //
    // enable_power_save is gated on CONFIG_TEMPY_SLEEP_BETWEEN_READINGS
    // for two reasons:
    //   1. When we're not sleeping, there's nothing to wake from, so
    //      pay-per-press with a permanently-running 5 ms scan timer is
    //      not worse than the interrupt-driven path.
    //   2. On ESP32-H2 with CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP,
    //      the button component's power-save path calls gpio_hold_en(),
    //      which hands GPIO 9 to the LP AON domain. H2's LP IO has no
    //      IOMUX, so the regular GPIO matrix ISR that iot_button relies
    //      on never sees level changes and the button appears dead.
    button_handle_t push_button = nullptr;
    const button_config_t btn_config = {};
    const button_gpio_config_t gpio_config = {
        .gpio_num = BSP_BUTTON_1_IO,
        .active_level = CONFIG_BSP_BUTTON_1_LEVEL,
#if CONFIG_TEMPY_SLEEP_BETWEEN_READINGS
        .enable_power_save = true,
#else
        .enable_power_save = false,
#endif
        .disable_pull = false,
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_config, &gpio_config, &push_button);
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
#if CONFIG_TEMPY_SLEEP_BETWEEN_READINGS
        .light_sleep_enable = true
#else
        .light_sleep_enable = false
#endif
    };

    ESP_LOGI(TAG, "Initializing power management...");
#if CONFIG_TEMPY_SLEEP_BETWEEN_READINGS
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

    if (err == ESP_OK && node != nullptr) {
        ESP_LOGI(TAG, "Enriching root node clusters...");
        ESP_ERROR_CHECK_WITHOUT_ABORT(enrich_root_node_clusters(node));
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

    // Initialize the sensor BEFORE esp_matter::start(). Two reasons:
    //   1. bme280_app_init caches the endpoint IDs it will later report to,
    //      so the endpoints must already be defined on the node (they are,
    //      above) and the sensor task must be constructed with those IDs
    //      before Matter starts servicing subscription requests.
    //   2. The sensor task itself vTaskDelays briefly on entry, so it does
    //      not race the CHIP stack coming up. Neither has_matter_subscribers
    //      nor attribute::update is safe to call before esp_matter::start()
    //      has completed initialization; the 500 ms delay in the task
    //      body handles that. If future refactors move esp_matter::start()
    //      much later than this point, revisit that delay.
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

    /* Log the setup payload so both the QR-code URL and the 11-digit
     * manual pairing code appear in the boot log, e.g.:
     *   chip[SVR]: SetupQRCode: [MT:...]
     *   chip[SVR]: https://project-chip.github.io/.../qrcode.html?data=MT%3A...
     *   chip[SVR]: Manual pairing code: [...]
     * The rendezvous flag needs to match the flavors of commissioning
     * this build supports; we're BLE-only for initial provisioning. */
    if (err == ESP_OK) {
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    }
}
