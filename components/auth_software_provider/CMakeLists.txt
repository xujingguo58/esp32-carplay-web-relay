idf_component_register(
    SRCS "auth_software_provider.c"
    INCLUDE_DIRS "include"
    REQUIRES auth_port log mbedtls
)

target_add_binary_data(${COMPONENT_LIB} "assets/identity.pk8" BINARY)
target_add_binary_data(${COMPONENT_LIB} "assets/certificate.p7b" BINARY)
