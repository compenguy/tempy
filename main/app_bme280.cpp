#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_matter_core.h>
#include <app/InteractionModelEngine.h>
#include <math.h> // required for NAN constant

#include "app_priv.h"
#include "bme280.h"

#define TAG_BME280 "APP_BME280"

#define I2C_PORT -1 // autoselect

#define I2C_BME_ADDRESS CONFIG_TEMPY_BME280_I2C_ADDRESS
#define I2C_BME_CLK_FREQ 50000

// Only push a Matter attribute update when the reading has moved by at
// least the hysteresis. Matter's subscription MaxInterval still forces
// periodic reports at the SDK layer, so subscribers won't miss data during
// long stretches of stable readings. Configured via menuconfig; Kconfig
// stores them as tenths-of-unit integers, which we convert to float here.
#define TEMP_HYSTERESIS_C       (CONFIG_TEMPY_TEMP_REPORT_HYSTERESIS_TENTHS_C / 10.0f)
#define HUMIDITY_HYSTERESIS_PCT (CONFIG_TEMPY_HUMIDITY_REPORT_HYSTERESIS_TENTHS_PCT / 10.0f)

// esp i2c handles
static i2c_master_bus_handle_t bus_handle;
static bme280_t bme;

// matter data
static uint16_t temp_endpoint_id;
static uint16_t humidity_endpoint_id;

// Last values pushed to Matter. NAN means "never reported"; the first
// successful reading always gets sent as a seed so that late-arriving
// controllers see a real attribute value on their initial Read.
static float last_reported_temp = NAN;
static float last_reported_humidity = NAN;

// matter callbacks
using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

// Application cluster specification, 7.18.2.11. Temperature
// represents a temperature on the Celsius scale with a resolution of 0.01°C.
// temp = (temperature in °C) x 100
static void matter_temp_sensor_notification(uint16_t endpoint_id, float temp)
{
    // Schedule the attribute update onto the Matter thread. ScheduleLambda is
    // thread-safe, so we intentionally do not take the CHIP stack lock here.
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, temp]() {
        attribute_t *attribute = attribute::get(endpoint_id,
                                                TemperatureMeasurement::Id,
                                                TemperatureMeasurement::Attributes::MeasuredValue::Id);
        if (attribute == nullptr) {
            ESP_LOGW(TAG_BME280, "Temperature attribute not found on endpoint %u", endpoint_id);
            return;
        }

        esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
        attribute::get_val(attribute, &val);
        val.val.i16 = static_cast<int16_t>(temp * 100);

        attribute::update(endpoint_id, TemperatureMeasurement::Id,
                          TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

// Application cluster specification, 2.6.4.1. MeasuredValue Attribute
// represents the humidity in percent.
// humidity = (humidity in %) x 100
static void matter_humidity_sensor_notification(uint16_t endpoint_id, float humidity)
{
    // Schedule the attribute update onto the Matter thread. See note above.
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, humidity]() {
        attribute_t *attribute = attribute::get(endpoint_id,
                                                RelativeHumidityMeasurement::Id,
                                                RelativeHumidityMeasurement::Attributes::MeasuredValue::Id);
        if (attribute == nullptr) {
            ESP_LOGW(TAG_BME280, "Humidity attribute not found on endpoint %u", endpoint_id);
            return;
        }

        esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
        attribute::get_val(attribute, &val);
        val.val.u16 = static_cast<uint16_t>(humidity * 100);

        attribute::update(endpoint_id, RelativeHumidityMeasurement::Id,
                          RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

// Push null to the temperature MeasuredValue. The attribute is nullable per
// the Matter Application Cluster spec, so pushing null tells subscribers
// "reading unavailable" rather than leaving stale data on the wire.
static void matter_temp_sensor_notification_null(uint16_t endpoint_id)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id]() {
        esp_matter_attr_val_t val = esp_matter_nullable_int16(nullable<int16_t>());
        attribute::update(endpoint_id, TemperatureMeasurement::Id,
                          TemperatureMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

// Push null to the humidity MeasuredValue. See temperature counterpart.
static void matter_humidity_sensor_notification_null(uint16_t endpoint_id)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id]() {
        esp_matter_attr_val_t val = esp_matter_nullable_uint16(nullable<uint16_t>());
        attribute::update(endpoint_id, RelativeHumidityMeasurement::Id,
                          RelativeHumidityMeasurement::Attributes::MeasuredValue::Id, &val);
    });
}

static esp_err_t i2c_master_init(void)
{
    // Create I2C master bus
    i2c_master_bus_config_t i2c_mst_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = (gpio_num_t)CONFIG_TEMPY_BME280_I2C_SDA_PIN,
        .scl_io_num = (gpio_num_t)CONFIG_TEMPY_BME280_I2C_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags {
            // Set this to false if you've wired suitable pullups to scl/sda
            // note that the internal pullup is not strong enough for high speed operation
            .enable_internal_pullup = true,
            .allow_pd = true,
        }
    };

    ESP_LOGD(TAG_BME280, "Attaching to primary I2C bus...");
    esp_err_t err = i2c_new_master_bus(&i2c_mst_config, &bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BME280, "Failed to create I2C master bus: %s", esp_err_to_name(err));
        return err;
    }

    // Probe for BME device on bus before continuing
    ESP_LOGI(TAG_BME280, "Probing for BME280 on I2C bus at 0x%02X...", I2C_BME_ADDRESS);
    err = i2c_master_probe(bus_handle, I2C_BME_ADDRESS, 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BME280, "BME280 not found at 0x%02X! Check wiring (SDA=%d, SCL=%d). Error: %s",
                 I2C_BME_ADDRESS, CONFIG_TEMPY_BME280_I2C_SDA_PIN, CONFIG_TEMPY_BME280_I2C_SCL_PIN,
                 esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG_BME280, "BME280 detected on I2C bus");
    return ESP_OK;
}

// True if the new reading differs from the previously-reported value by at
// least the hysteresis, or if we have never reported. NAN readings never
// pass this gate.
static bool reading_changed_enough(float new_val, float last_val, float hysteresis)
{
    if (isnan(new_val)) {
        return false;
    }
    if (isnan(last_val)) {
        return true;
    }
    return fabsf(new_val - last_val) >= hysteresis;
}

// Check whether any Matter subscriber currently has a subscription open on
// this node. GetNumActiveReadHandlers walks the linked list of read
// handlers, which the Matter thread modifies when subscriptions are
// created or torn down, so we take the CHIP stack lock while querying.
static bool has_matter_subscribers(void)
{
    // RAII scope lock: acquires the CHIP stack mutex on construction and
    // releases it on destruction, tolerating both SUCCESS and
    // ALREADY_TAKEN (reentrant) cases automatically. portMAX_DELAY means
    // the take blocks indefinitely, so FAILED is not reachable here in
    // practice; if we ever bound the wait, the wrapper still skips the
    // unlock on failure so nothing leaks.
    lock::ScopedChipStackLock chip_lock(portMAX_DELAY);
    return chip::app::InteractionModelEngine::GetInstance()
        ->GetNumActiveReadHandlers(chip::app::ReadHandler::InteractionType::Subscribe) > 0;
}

// Retry parameters for transient I2C failures. A single failure will
// typically be a NACK (esp_err ESP_ERR_INVALID_STATE from the IDF v5
// i2c_master driver) caused by a µs-scale rail droop when the radio is
// bursting, or by noise coupling into the SDA/SCL lines. Retrying after
// a short delay clears these almost always. Three attempts × 20 ms is
// negligible against a 60 s cycle.
#define BME280_READ_MAX_ATTEMPTS  3
#define BME280_READ_RETRY_DELAY_MS 20

static void task_bme280_forced_mode(void *ignore)
{
    float t, h;
    // Track how many read cycles in a row have failed and whether we have
    // already emitted null to Matter for the current outage. Nulling once
    // (rather than every cycle) keeps us from repeatedly waking the Thread
    // radio while the sensor is offline. See CONFIG_TEMPY_NULL_AFTER_FAILED_CYCLES.
    unsigned consecutive_failed_cycles = 0;
    bool reported_null = false;
    // Allow some time for the Matter thread to finish initializing before
    // scheduling the first attribute update. (See init-order comment in
    // bme280_app_init below for why this delay is needed.)
    vTaskDelay(pdMS_TO_TICKS(500));
    while (1) {
        esp_err_t err = ESP_FAIL;
        for (int attempt = 0; attempt < BME280_READ_MAX_ATTEMPTS; ++attempt) {
            err = bme280_read_temp_humidity(&bme, &t, &h);
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGD(TAG_BME280, "Read attempt %d/%d failed (%s), retrying",
                     attempt + 1, BME280_READ_MAX_ATTEMPTS, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(BME280_READ_RETRY_DELAY_MS));
        }
        if (err == ESP_OK) {
            // Recovery path: if we had previously reported null, clear the
            // hysteresis-tracking state so this first good reading is
            // treated as a fresh seed and hits the wire immediately,
            // regardless of how close it is to the pre-outage value.
            if (reported_null) {
                ESP_LOGI(TAG_BME280, "Sensor recovered after %u failed cycles", consecutive_failed_cycles);
                last_reported_temp = NAN;
                last_reported_humidity = NAN;
                reported_null = false;
            }
            consecutive_failed_cycles = 0;

            ESP_LOGD(TAG_BME280, "Temperature: %.2f C", t);
            ESP_LOGD(TAG_BME280, "Humidity:    %.1f %%RH", h);

            // Matter attribute updates wake the CHIP stack and hold the
            // ICD in active mode for ActiveModeThreshold. We push in
            // three cases:
            //   1. First successful reading, regardless of subscribers:
            //      this "seeds" the local attribute so a controller
            //      that Reads before Subscribing sees a real value.
            //   2. There is at least one subscriber AND the reading has
            //      moved by at least the hysteresis.
            //   3. (Not handled here) Matter's own MaxInterval logic
            //      forces the SDK to send a report during long stretches
            //      of stable readings; that happens without our help.
            bool subscribed = has_matter_subscribers();

            // "first" is gated on a non-NAN reading so a bogus sensor
            // value can never poison the seed path.
            bool first_temp = isnan(last_reported_temp) && !isnan(t);
            bool temp_changed = reading_changed_enough(t, last_reported_temp, TEMP_HYSTERESIS_C);
            if (first_temp || (subscribed && temp_changed)) {
                matter_temp_sensor_notification(temp_endpoint_id, t);
                last_reported_temp = t;
                if (first_temp) {
                    ESP_LOGI(TAG_BME280, "Seeded temperature attribute with %.2f C", t);
                }
            } else if (!subscribed) {
                ESP_LOGD(TAG_BME280, "No subscribers, skipping temperature update");
            } else {
                ESP_LOGD(TAG_BME280, "Temperature unchanged (%.2f C), skipping update", t);
            }

            bool first_hum = isnan(last_reported_humidity) && !isnan(h);
            bool hum_changed = reading_changed_enough(h, last_reported_humidity, HUMIDITY_HYSTERESIS_PCT);
            if (first_hum || (subscribed && hum_changed)) {
                matter_humidity_sensor_notification(humidity_endpoint_id, h);
                last_reported_humidity = h;
                if (first_hum) {
                    ESP_LOGI(TAG_BME280, "Seeded humidity attribute with %.1f %%RH", h);
                }
            } else if (!subscribed) {
                ESP_LOGD(TAG_BME280, "No subscribers, skipping humidity update");
            } else {
                ESP_LOGD(TAG_BME280, "Humidity unchanged (%.1f %%RH), skipping update", h);
            }
        } else {
            ++consecutive_failed_cycles;
            ESP_LOGW(TAG_BME280, "Read failed after %d attempts: %s (%u consecutive cycles)",
                     BME280_READ_MAX_ATTEMPTS, esp_err_to_name(err), consecutive_failed_cycles);
            // Push null exactly once per outage, right when we cross the
            // threshold. Repeating the null every cycle would wake the
            // Thread radio for no incremental information.
            if (!reported_null && consecutive_failed_cycles >= CONFIG_TEMPY_NULL_AFTER_FAILED_CYCLES) {
                ESP_LOGW(TAG_BME280, "Reporting sensor unavailable to Matter after %u consecutive failures",
                         consecutive_failed_cycles);
                matter_temp_sensor_notification_null(temp_endpoint_id);
                matter_humidity_sensor_notification_null(humidity_endpoint_id);
                last_reported_temp = NAN;
                last_reported_humidity = NAN;
                reported_null = true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_TEMPY_DELAY_BETWEEN_SENSOR_READINGS_MS));
    }
}

void bme280_app_init(endpoint_t *temp_sensor_ep, endpoint_t *humidity_sensor_ep)
{
    // Initialization order note: this function runs before esp_matter::start()
    // in app_main. That's intentional -- Matter endpoints need to be defined
    // before the CHIP stack is started, and the sensor task must know the
    // endpoint IDs. The task itself vTaskDelays 500 ms before its first
    // reading, giving esp_matter::start() (called immediately after us) time
    // to bring the CHIP stack up before we hit it with the first
    // has_matter_subscribers() query or attribute::update. If you ever move
    // esp_matter::start() much later relative to bme280_app_init, revisit
    // that delay.

    // init for sending sensor data via matter+thread
    temp_endpoint_id = endpoint::get_id(temp_sensor_ep);
    humidity_endpoint_id = endpoint::get_id(humidity_sensor_ep);

    // init sensor on i2c. If the sensor isn't wired or the bus fails, log
    // and continue: the app will still run as a bare Matter node that
    // never reports readings, which is more useful for debugging than a
    // crash loop.
    if (i2c_master_init() != ESP_OK) {
        ESP_LOGE(TAG_BME280, "I2C initialization failed; sensor task will not start");
        return;
    }
    ESP_LOGI(TAG_BME280, "Initializing BME280...");
    esp_err_t err = bme280_init(&bme, bus_handle, I2C_BME_ADDRESS, I2C_BME_CLK_FREQ);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BME280, "BME280 init failed (%s); sensor task will not start",
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG_BME280, "Scheduling BME280 to take regular readings...");
    BaseType_t rc = xTaskCreate(&task_bme280_forced_mode, "bme280_forced_mode",
                                3072, nullptr, 6, nullptr);
    if (rc != pdPASS) {
        ESP_LOGE(TAG_BME280, "Failed to create BME280 task (rc=%d)", (int)rc);
    }
}
