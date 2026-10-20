idf_component_register(
    SRCS "airplay_screen_codec.c" "airplay_screen_stream.c" "airplay_screen_server.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls lwip net_port time_port
)
