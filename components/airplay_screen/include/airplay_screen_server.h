/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "airplay_screen.h"

#define AIRPLAY_SCREEN_SERVER_MAX_BODY_BYTES (8U * 1024U * 1024U)

typedef void (*airplay_screen_server_exit_cb_t)(void *context);

/*
 * Bind a bounded TCP Screen DataStream listener. One output key is scoped to
 * one accepted TCP peer and one nonce-counter epoch. Unexpected listener or
 * peer exit invokes on_unexpected_exit from the server task; that callback
 * must not synchronously stop this server.
 */
esp_err_t airplay_screen_server_start(uint16_t requested_port,
                                      const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                      size_t max_body_bytes,
                                      airplay_screen_message_cb_t callback,
                                      void *context,
                                      airplay_screen_server_exit_cb_t on_unexpected_exit,
                                      void *exit_context,
                                      uint16_t *out_port);
void airplay_screen_server_stop(void);
