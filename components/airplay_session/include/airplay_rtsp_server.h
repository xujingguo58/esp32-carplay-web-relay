/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "airplay_rtsp_session.h"
#include "esp_err.h"

#define AIRPLAY_RTSP_DEFAULT_PORT 7000U

typedef struct airplay_rtsp_server airplay_rtsp_server_t;

typedef void (*airplay_rtsp_event_state_cb_t)(bool connected, void *context);

typedef struct {
    airplay_rtsp_pairing_config_t pairing;
    const uint8_t *info_plist;
    size_t info_plist_len;
    uint16_t control_port; /* 0 selects port 7000 */
    uint16_t screen_port;  /* 0 requests an ephemeral screen-data port */
    airplay_rtsp_event_state_cb_t on_event_state;
    void *event_state_context;
} airplay_rtsp_server_config_t;

/* Starts the one-client RTSP listener on the current SoftAP IPv4 address.
 * Pairing identity callbacks and the binary-plist /info capability document
 * are provided by the authorized deployment layer. */
esp_err_t airplay_rtsp_server_start(
    const airplay_rtsp_server_config_t *config,
    airplay_rtsp_server_t **out_server);
uint16_t airplay_rtsp_server_get_port(const airplay_rtsp_server_t *server);
bool airplay_rtsp_server_is_running(void);

/* Requests shutdown and waits for the listener task to release all sessions. */
void airplay_rtsp_server_stop(airplay_rtsp_server_t *server);
