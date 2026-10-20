idf_component_register(
    SRCS "airplay_software_provider.c"
    INCLUDE_DIRS "include"
    REQUIRES airplay_identity_port airplay_pairing_store airplay_control
             ed25519 esp_common esp_hw_support log mbedtls nvs_flash
)

target_add_binary_data(${COMPONENT_LIB} "assets/info.plist" BINARY)
