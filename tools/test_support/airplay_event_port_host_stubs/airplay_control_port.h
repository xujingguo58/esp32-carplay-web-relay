#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_control_cipher.h"
#include "airplay_pair_verify.h"
#include "esp_err.h"

typedef bool (*airplay_control_write_cb_t)(const uint8_t *bytes, size_t len,
                                           void *context);

esp_err_t airplay_control_port_init(void);
esp_err_t airplay_control_port_attach(
    const uint8_t read_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    const uint8_t write_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    uint32_t stream_id, uint32_t initial_cseq,
    airplay_control_write_cb_t write_cb, void *context);
void airplay_control_port_detach(void);
esp_err_t airplay_control_port_receive_encrypted(const uint8_t *bytes,
                                                  size_t len);
