idf_component_register(
    SRCS "airplay_timing.c" "airplay_timing_port.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common esp_event esp_netif freertos heap log lwip net_port time_port
)
