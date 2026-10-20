idf_component_register(
    SRCS "bt_port.c"
    INCLUDE_DIRS "include"
    REQUIRES
        bt
        esp_common
        freertos
        log
)
