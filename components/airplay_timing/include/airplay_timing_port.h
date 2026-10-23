/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct airplay_timing_port airplay_timing_port_t;

typedef struct {
    uint32_t peer_ipv4; /* Network byte order. */
    uint16_t peer_port; /* Zero disables periodic clock requests. */
} airplay_timing_port_config_t;

/* Starts a bounded timing exchange endpoint bound to the active SoftAP IPv4. */
esp_err_t airplay_timing_port_start(const airplay_timing_port_config_t *config,
                                    airplay_timing_port_t **out_port);
uint16_t airplay_timing_port_get_port(const airplay_timing_port_t *port);
int airplay_timing_port_get_socket_fd(const airplay_timing_port_t *port);
void airplay_timing_port_poll(airplay_timing_port_t *port, bool socket_readable);
void airplay_timing_port_stop(airplay_timing_port_t *port);
