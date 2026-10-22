#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* The implementation supplies authorized certificate/key operations; no keys are stored here. */
typedef struct {
    esp_err_t (*read_certificate)(uint8_t *out, size_t *inout_len, void *context);
    esp_err_t (*sign_challenge)(const uint8_t *challenge,
                                size_t challenge_len,
                                uint8_t *signature,
                                size_t *inout_signature_len,
                                void *context);
    uint8_t protocol_major;
    void *context;
} auth_port_provider_t;

esp_err_t auth_port_register_provider(const auth_port_provider_t *provider);
esp_err_t auth_port_read_certificate(uint8_t *out, size_t *inout_len);
esp_err_t auth_port_sign_challenge(const uint8_t *challenge,
                                   size_t challenge_len,
                                   uint8_t *signature,
                                   size_t *inout_signature_len);
esp_err_t auth_port_get_protocol_major(uint8_t *protocol_major);
int auth_port_is_configured(void);

