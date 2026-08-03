set(priv_requires esp_driver_i2c esp_matter app_reset)
if(CONFIG_ENABLE_BLE_BEACON)
    list(APPEND priv_requires ble_beacon)
endif()

idf_component_register(SRC_DIRS          "."
                       PRIV_INCLUDE_DIRS "." "${ESP_MATTER_PATH}/examples/common/utils"
                       PRIV_REQUIRES     ${priv_requires})

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
