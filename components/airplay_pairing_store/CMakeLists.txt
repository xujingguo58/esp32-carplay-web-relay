idf_component_register(
    SRCS "airplay_pairing_store.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_pairing esp_common freertos nvs_flash
)
