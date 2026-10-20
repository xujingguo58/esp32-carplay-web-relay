idf_component_register(
    SRCS "media_bridge.c" "media_timestamp.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "test_stream.bin"
    REQUIRES
        esp_common
        esp_peer
        freertos
        heap
        log
        mbedtls
        time_port
)
