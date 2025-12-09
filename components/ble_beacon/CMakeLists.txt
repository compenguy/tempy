idf_component_register(
    SRCS "ble_beacon.c" "gap.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt
)
