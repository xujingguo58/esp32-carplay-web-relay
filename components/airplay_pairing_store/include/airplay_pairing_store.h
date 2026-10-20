/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS 8U
#define AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES 64U
#define AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES 32U

/* Initialize after nvs_flash_init(). Stored controller identifiers and public keys
 * are public pairing metadata; accessory/MFi private keys are never stored here. */
esp_err_t airplay_pairing_store_init(void);

/* These callbacks can be passed directly to Pair-Setup and Pair-Verify providers. */
bool airplay_pairing_store_save_callback(const uint8_t *identifier,
                                         size_t identifier_len,
                                         const uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
                                         void *context);
bool airplay_pairing_store_lookup_callback(
    const uint8_t *identifier,
    size_t identifier_len,
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context);

esp_err_t airplay_pairing_store_remove(const uint8_t *identifier,
                                       size_t identifier_len);
esp_err_t airplay_pairing_store_clear(void);
esp_err_t airplay_pairing_store_count(size_t *out_count);
