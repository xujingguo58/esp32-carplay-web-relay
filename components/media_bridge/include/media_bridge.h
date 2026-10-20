#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef esp_err_t (*media_bridge_signal_cb_t)(const char *type,
                                               const char *payload,
                                               void *context);

esp_err_t media_bridge_start(media_bridge_signal_cb_t signal_cb, void *context);
esp_err_t media_bridge_handle_answer(const char *sdp, size_t sdp_len);
esp_err_t media_bridge_set_h264_config(const uint8_t *annex_b_parameter_sets, size_t size);
esp_err_t media_bridge_send_h264(const uint8_t *annex_b_access_unit,
                                 size_t size,
                                 uint64_t sender_time_ns,
                                 uint64_t arrival_time_us);
void media_bridge_stop(void);
bool media_bridge_is_connected(void);
uint32_t media_bridge_frames_sent(void);
uint32_t media_bridge_frames_dropped(void);
const char *media_bridge_state(void);
