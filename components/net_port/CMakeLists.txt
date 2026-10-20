idf_component_register(
    SRCS "net_port.c"
    INCLUDE_DIRS "include"
    REQUIRES
        esp_common
        esp_event
        esp_netif
        esp_wifi
        freertos
        log
)
