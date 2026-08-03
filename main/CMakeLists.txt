# ble_beacon is listed unconditionally: ESP-IDF resolves PRIV_REQUIRES
# during an early expansion phase where CONFIG_* is not yet defined, so a
# `if(CONFIG_TEMPY_ENABLE_BLE_BEACON)` guard here would silently drop the
# dependency at graph-build time and then fail at compile time when the
# source file's #if pulls in the header. The runtime #if guards in
# app_bme280.cpp keep the calls out; --gc-sections (enabled by default in
# ESP-IDF) then drops the unused ble_beacon code from the final image.
idf_component_register(SRC_DIRS          "."
                       PRIV_INCLUDE_DIRS "." "${ESP_MATTER_PATH}/examples/common/utils"
                       PRIV_REQUIRES     esp_driver_i2c
                                         esp_matter
                                         app_reset
                                         ble_beacon)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
