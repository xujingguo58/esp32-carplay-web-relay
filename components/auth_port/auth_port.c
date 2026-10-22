/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This port retains provider callbacks only; key storage and signing are implemented by
 * provider components. It does not bypass the iAP2 challenge-response exchange.
 */
#include "auth_port.h"

#include <string.h>

static auth_port_provider_t s_provider;
static int s_registered;

esp_err_t auth_port_register_provider(const auth_port_provider_t *provider)
{
    if (provider == NULL || provider->read_certificate == NULL || provider->sign_challenge == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_provider = *provider;
    s_registered = 1;
    return ESP_OK;
}

esp_err_t auth_port_read_certificate(uint8_t *out, size_t *inout_len)
{
    if (out == NULL || inout_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registered) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_provider.read_certificate(out, inout_len, s_provider.context);
}

esp_err_t auth_port_sign_challenge(const uint8_t *challenge,
                                   size_t challenge_len,
                                   uint8_t *signature,
                                   size_t *inout_signature_len)
{
    if (challenge == NULL || challenge_len == 0 || signature == NULL || inout_signature_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_registered) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    return s_provider.sign_challenge(challenge, challenge_len, signature,
                                     inout_signature_len, s_provider.context);
}

esp_err_t auth_port_get_protocol_major(uint8_t *protocol_major)
{
    if (protocol_major == NULL) return ESP_ERR_INVALID_ARG;
    if (!s_registered) return ESP_ERR_NOT_SUPPORTED;
    *protocol_major = s_provider.protocol_major;
    return ESP_OK;
}

int auth_port_is_configured(void)
{
    return s_registered;
}
