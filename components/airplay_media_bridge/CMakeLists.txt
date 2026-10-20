idf_component_register(
    SRCS "airplay_media_bridge.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_pairing airplay_rtsp airplay_screen media_bridge esp_common mbedtls
)
