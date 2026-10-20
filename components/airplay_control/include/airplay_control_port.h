/* SPDX-License-Identifier: GPL-3.0-or-later; AirPlay ControlCipher framing reference: DiPlay */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "airplay_control_cipher.h"
#include "airplay_rtsp_codec.h"

#define AIRPLAY_CONTROL_KEY_BYTES 32U

typedef bool (*airplay_control_write_cb_t)(const uint8_t *bytes,
                                           size_t len,
                                           void *context);

/* Initialize once before the web gateway starts. */
esp_err_t airplay_control_port_init(void);

/* Attach only after the AirPlay event socket and Pair-Verify event keys are
 * ready. Calls are serialized; write_cb runs while the session lock is held. */
esp_err_t airplay_control_port_attach(
                                      const uint8_t read_key[AIRPLAY_CONTROL_KEY_BYTES],
                                      const uint8_t write_key[AIRPLAY_CONTROL_KEY_BYTES],
                                      uint32_t stream_id,
                                      uint32_t initial_cseq,
                                      airplay_control_write_cb_t write_cb,
                                      void *context);
void airplay_control_port_detach(void);

/* Feed TCP bytes received from the event socket. Complete RTSP requests are
 * answered with encrypted 200 responses; RTSP/HTTP responses are consumed.
 * Malformed or unauthenticated input detaches the session. */
esp_err_t airplay_control_port_receive_encrypted(const uint8_t *bytes,
                                                 size_t len);

bool airplay_control_port_send_hid_report(uint32_t hid_uid,
                                          const uint8_t *report,
                                          size_t report_len,
                                          void *context);

/* Send DiPlay's main-screen forceKeyFrame event command, rate limited per event session. */
bool airplay_control_port_request_main_screen_keyframe(void);
