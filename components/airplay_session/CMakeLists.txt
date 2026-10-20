idf_component_register(
    SRCS "airplay_rtsp_session.c" "airplay_rtsp_server.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_control airplay_media_bridge airplay_pairing airplay_rtsp airplay_timing
            esp_common freertos heap log lwip mbedtls net_port
)
