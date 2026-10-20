/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_media_bridge.h"
#include "media_bridge.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t bytes[64];
    size_t len;
    unsigned config_calls;
    unsigned frame_calls;
    uint64_t sender_time_ns;
    uint64_t arrival_time_us;
} media_capture_t;

static media_capture_t s_capture;

esp_err_t media_bridge_set_h264_config(const uint8_t *parameter_sets, size_t size)
{
    assert(size <= sizeof(s_capture.bytes));
    assert(parameter_sets != NULL || size == 0U);
    if (size != 0U) memcpy(s_capture.bytes, parameter_sets, size);
    s_capture.len = size;
    ++s_capture.config_calls;
    return ESP_OK;
}

esp_err_t media_bridge_send_h264(const uint8_t *access_unit,
                                 size_t size,
                                 uint64_t sender_time_ns,
                                 uint64_t arrival_time_us)
{
    assert(size <= sizeof(s_capture.bytes));
    assert(access_unit != NULL || size == 0U);
    if (size != 0U) memcpy(s_capture.bytes, access_unit, size);
    s_capture.len = size;
    s_capture.sender_time_ns = sender_time_ns;
    s_capture.arrival_time_us = arrival_time_us;
    ++s_capture.frame_calls;
    return ESP_OK;
}

int main(void)
{
    static const uint8_t parameter_sets[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1e,
        0x00, 0x00, 0x00, 0x01, 0x68, 0xce, 0x06, 0xe2,
    };
    static const uint8_t access_unit[] = {
        0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84,
    };

    assert(airplay_media_bridge_on_screen_message(
               AIRPLAY_SCREEN_MESSAGE_CONFIG_H264, parameter_sets,
               sizeof(parameter_sets), 0U, 100U, NULL) == ESP_OK);
    assert(s_capture.config_calls == 1U && s_capture.frame_calls == 0U);
    assert(s_capture.len == sizeof(parameter_sets));
    assert(memcmp(s_capture.bytes, parameter_sets, sizeof(parameter_sets)) == 0);

    assert(airplay_media_bridge_on_screen_message(
               AIRPLAY_SCREEN_MESSAGE_FRAME_H264, access_unit,
               sizeof(access_unit), 987654321U, 123456U, NULL) == ESP_OK);
    assert(s_capture.config_calls == 1U && s_capture.frame_calls == 1U);
    assert(s_capture.len == sizeof(access_unit));
    assert(memcmp(s_capture.bytes, access_unit, sizeof(access_unit)) == 0);
    assert(s_capture.sender_time_ns == 987654321U);
    assert(s_capture.arrival_time_us == 123456U);

    assert(airplay_media_bridge_on_screen_message(
               (airplay_screen_message_type_t)99, access_unit,
               sizeof(access_unit), 1U, 2U, NULL) == ESP_ERR_NOT_SUPPORTED);
    assert(airplay_media_bridge_on_screen_message(
               AIRPLAY_SCREEN_MESSAGE_FRAME_H264, NULL, 1U,
               1U, 2U, NULL) == ESP_ERR_INVALID_ARG);
    assert(airplay_media_bridge_on_screen_message(
               AIRPLAY_SCREEN_MESSAGE_CONFIG_H264, access_unit, 0U,
               1U, 2U, NULL) == ESP_ERR_INVALID_ARG);
    assert(s_capture.config_calls == 1U && s_capture.frame_calls == 1U);

    puts("airplay media bridge host test passed");
    return 0;
}
