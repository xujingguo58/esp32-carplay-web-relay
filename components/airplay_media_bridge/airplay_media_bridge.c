/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_media_bridge.h"

#include "airplay_pair_verify.h"
#include "media_bridge.h"
#include "mbedtls/platform_util.h"

esp_err_t airplay_media_bridge_on_screen_message(airplay_screen_message_type_t type,
                                                 uint8_t *annex_b,
                                                 size_t annex_b_len,
                                                 size_t prefix_capacity,
                                                 uint64_t sender_time_ns,
                                                 uint64_t arrival_time_us,
                                                 void *context)
{
    (void)context;
    if (annex_b == NULL || annex_b_len == 0) return ESP_ERR_INVALID_ARG;

    switch (type) {
    case AIRPLAY_SCREEN_MESSAGE_CONFIG_H264:
        return media_bridge_set_h264_config(annex_b, annex_b_len);
    case AIRPLAY_SCREEN_MESSAGE_FRAME_H264:
        return media_bridge_send_h264_borrowed(annex_b, prefix_capacity,
                                               annex_b_len, sender_time_ns,
                                               arrival_time_us);
    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t airplay_media_bridge_start_screen_stream(
    const airplay_rtsp_pairing_t *pairing,
    uint64_t stream_connection_id,
    uint16_t requested_port,
    airplay_screen_server_exit_cb_t on_unexpected_exit,
    void *exit_context,
    uint16_t *out_port)
{
    if (pairing == NULL || out_port == NULL) return ESP_ERR_INVALID_ARG;
    *out_port = 0U;
    uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES];
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_pairing_derive_screen_output_key(
            pairing, stream_connection_id, output_key);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK) {
        mbedtls_platform_zeroize(output_key, sizeof(output_key));
        return key_result == AIRPLAY_PAIR_VERIFY_STATE_ERROR
                   ? ESP_ERR_INVALID_STATE : ESP_ERR_INVALID_ARG;
    }
    esp_err_t result = airplay_screen_server_start(
        requested_port, output_key, AIRPLAY_SCREEN_SERVER_MAX_BODY_BYTES,
        airplay_media_bridge_on_screen_message, NULL,
        on_unexpected_exit, exit_context, out_port);
    mbedtls_platform_zeroize(output_key, sizeof(output_key));
    return result;
}

void airplay_media_bridge_stop_screen_stream(void)
{
    airplay_screen_server_stop();
}
