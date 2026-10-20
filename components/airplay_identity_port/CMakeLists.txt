idf_component_register(
    SRCS "airplay_identity_port.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_control airplay_rtsp airplay_session esp_common
)
