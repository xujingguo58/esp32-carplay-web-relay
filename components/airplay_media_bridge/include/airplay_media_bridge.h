/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "airplay_rtsp_pairing.h"
#include "airplay_screen.h"
#include "airplay_screen_server.h"

/* Callback adapter for airplay_screen_server_start(). Configuration messages
 * update the WebRTC sender's SPS/PPS; decrypted access units enter its bounded
 * send queue. The caller must stop the screen server before stopping media. */
esp_err_t airplay_media_bridge_on_screen_message(airplay_screen_message_type_t type,
                                                 const uint8_t *annex_b,
                                                 size_t annex_b_len,
                                                 uint64_t sender_time_ns,
                                                 uint64_t arrival_time_us,
                                                 void *context);

/* SETUP adapter: derive the output key from the private Pair-Verify session,
 * start the bounded Screen DataStream listener, and return its bound port. */
esp_err_t airplay_media_bridge_start_screen_stream(
    const airplay_rtsp_pairing_t *pairing,
    uint64_t stream_connection_id,
    uint16_t requested_port,
    airplay_screen_server_exit_cb_t on_unexpected_exit,
    void *exit_context,
    uint16_t *out_port);

void airplay_media_bridge_stop_screen_stream(void);
