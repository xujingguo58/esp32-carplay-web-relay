idf_component_register(
    SRCS "airplay_tlv8.c" "airplay_srp6a.c" "airplay_pair_setup.c" "airplay_pair_verify.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mbedtls
)
