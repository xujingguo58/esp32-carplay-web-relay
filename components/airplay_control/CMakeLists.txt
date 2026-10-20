idf_component_register(
    SRCS "airplay_control_port.c" "airplay_control_cipher.c" "airplay_control_stream.c" "airplay_event_port.c" "airplay_hid_plist.c" "airplay_command_plist.c" "airplay_hid_descriptor.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_pairing airplay_rtsp esp_common freertos heap log lwip mbedtls net_port
)
