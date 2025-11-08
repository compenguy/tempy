idf_component_register(SRC_DIRS          "." "driver"
                       PRIV_INCLUDE_DIRS "." "driver" "${ESP_MATTER_PATH}/examples/common/utils")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
