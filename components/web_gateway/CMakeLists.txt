idf_component_register(
    SRCS "web_gateway.c" "web_gateway_rate_limit.c" "web_gateway_pairing.c"
    INCLUDE_DIRS "include"
    REQUIRES
        airplay_control
        esp_common
        esp_http_server
        esp_netif
        esp_system
        esp_timer
        heap
        input_bridge
        json
        log
        media_bridge
        net_port
        time_port
)
