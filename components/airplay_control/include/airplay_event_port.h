/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "airplay_pair_verify.h"
#include "esp_err.h"

typedef struct airplay_event_port airplay_event_port_t;

typedef void (*airplay_event_port_state_cb_t)(bool connected, void *context);

typedef struct {
    airplay_pair_verify_event_keys_t keys;
    uint32_t stream_id;
    uint32_t initial_cseq;
    airplay_event_port_state_cb_t on_state;
    void *context;
} airplay_event_port_config_t;

/* Starts a one-controller TCP event listener on INADDR_ANY. The returned port
 * is intended for the AirPlay SETUP response. Keys are copied, then scrubbed
 * after the single accepted connection is attached to airplay_control_port. */
esp_err_t airplay_event_port_start(const airplay_event_port_config_t *config,
                                   airplay_event_port_t **out_port);

uint16_t airplay_event_port_get_port(const airplay_event_port_t *port);

/* Stops the listener/connection, detaches the ControlCipher session, and waits
 * for its task to finish. Do not call synchronously from on_state. */
void airplay_event_port_stop(airplay_event_port_t *port);
