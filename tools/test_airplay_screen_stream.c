/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "airplay_media_bridge.h"
#include "airplay_screen_server.h"
#include "media_bridge.h"
#include "mbedtls/chachapoly.h"

typedef struct {
    uint32_t calls;
    airplay_screen_message_type_t type;
    uint8_t bytes[32];
    size_t len;
    uint64_t sender_time_ns;
    uint64_t arrival_time_us;
    size_t prefix_capacity;
} callback_capture_t;

typedef struct {
    uint32_t config_calls;
    uint8_t config[32];
    size_t config_len;
    uint32_t frame_calls;
    uint8_t frame[32];
    size_t frame_len;
    uint64_t sender_time_ns;
    uint64_t arrival_time_us;
    uint8_t *frame_pointer;
    size_t prefix_capacity;
} media_bridge_capture_t;

static media_bridge_capture_t s_media_capture;

esp_err_t media_bridge_set_h264_config(const uint8_t *parameter_sets, size_t size)
{
    assert(parameter_sets != NULL && size > 0U && size <= sizeof(s_media_capture.config));
    memcpy(s_media_capture.config, parameter_sets, size);
    s_media_capture.config_len = size;
    ++s_media_capture.config_calls;
    return ESP_OK;
}

esp_err_t media_bridge_send_h264_borrowed(uint8_t *access_unit,
                                          size_t prefix_capacity,
                                          size_t size,
                                          uint64_t sender_time_ns,
                                          uint64_t arrival_time_us)
{
    assert(access_unit != NULL && size > 0U && size <= sizeof(s_media_capture.frame));
    memcpy(s_media_capture.frame, access_unit, size);
    s_media_capture.frame_len = size;
    s_media_capture.sender_time_ns = sender_time_ns;
    s_media_capture.arrival_time_us = arrival_time_us;
    s_media_capture.frame_pointer = access_unit;
    s_media_capture.prefix_capacity = prefix_capacity;
    ++s_media_capture.frame_calls;
    return ESP_OK;
}

static void put_le32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void put_be32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

static void put_le64(uint8_t *out, uint64_t value)
{
    for (size_t i = 0U; i < 8U; ++i) out[i] = (uint8_t)(value >> (8U * i));
}

static void make_nonce(uint64_t counter, uint8_t nonce[12])
{
    memset(nonce, 0, 4U);
    for (size_t i = 0U; i < 8U; ++i) {
        nonce[4U + i] = (uint8_t)(counter >> (8U * i));
    }
}

static size_t encrypt_frame(const uint8_t key[AIRPLAY_SCREEN_KEY_BYTES],
                            uint64_t counter,
                            const uint8_t *plaintext,
                            size_t plaintext_len,
                            uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES],
                            uint8_t *body)
{
    size_t body_len = plaintext_len + AIRPLAY_SCREEN_TAG_BYTES;
    memset(header, 0, AIRPLAY_SCREEN_HEADER_BYTES);
    put_le32(header, (uint32_t)body_len);
    header[4] = 0U;
    put_le64(header + 8U, (UINT64_C(2) << 32) | UINT64_C(0x80000000));
    uint8_t nonce[12];
    make_nonce(counter, nonce);
    mbedtls_chachapoly_context cipher;
    mbedtls_chachapoly_init(&cipher);
    assert(mbedtls_chachapoly_setkey(&cipher, key) == 0);
    assert(mbedtls_chachapoly_encrypt_and_tag(
               &cipher, plaintext_len, nonce, header, AIRPLAY_SCREEN_HEADER_BYTES,
               plaintext, body, body + plaintext_len) == 0);
    mbedtls_chachapoly_free(&cipher);
    return body_len;
}

static void make_config_header(size_t body_len,
                               uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES])
{
    memset(header, 0, AIRPLAY_SCREEN_HEADER_BYTES);
    put_le32(header, (uint32_t)body_len);
    header[4] = 1U;
    put_le64(header + 8U, (UINT64_C(1) << 32) | UINT64_C(0x80000000));
}

static esp_err_t capture_message(airplay_screen_message_type_t type,
                                 uint8_t *annex_b,
                                 size_t annex_b_len,
                                 size_t prefix_capacity,
                                 uint64_t sender_time_ns,
                                 uint64_t arrival_time_us,
                                 void *context)
{
    callback_capture_t *capture = context;
    assert(capture != NULL && annex_b != NULL && annex_b_len <= sizeof(capture->bytes));
    assert(airplay_media_bridge_on_screen_message(type, annex_b, annex_b_len,
                                                  prefix_capacity,
                                                  sender_time_ns, arrival_time_us,
                                                  NULL) == ESP_OK);
    ++capture->calls;
    capture->type = type;
    capture->len = annex_b_len;
    capture->prefix_capacity = prefix_capacity;
    capture->sender_time_ns = sender_time_ns;
    capture->arrival_time_us = arrival_time_us;
    memcpy(capture->bytes, annex_b, annex_b_len);
    return ESP_OK;
}

static esp_err_t discard_large_message(airplay_screen_message_type_t type,
                                       uint8_t *annex_b,
                                       size_t annex_b_len,
                                       size_t prefix_capacity,
                                       uint64_t sender_time_ns,
                                       uint64_t arrival_time_us,
                                       void *context)
{
    (void)sender_time_ns;
    (void)arrival_time_us;
    assert(type == AIRPLAY_SCREEN_MESSAGE_FRAME_H264);
    assert(annex_b != NULL && annex_b_len > 512U * 1024U);
    assert(prefix_capacity == AIRPLAY_SCREEN_CONFIG_BYTES);
    assert(context != NULL);
    ++*(uint32_t *)context;
    return ESP_OK;
}

int main(void)
{
    static const uint8_t key[AIRPLAY_SCREEN_KEY_BYTES] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
        0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f,
    };
    static const uint8_t plaintext[] = {0, 0, 0, 2, 0x65, 0xaa};
    static const uint8_t expected_annex_b[] = {0, 0, 0, 1, 0x65, 0xaa};
    static const uint8_t avcc[] = {
        0, 0, 0, 0, 'a', 'v', 'c', 'C',
        1, 100, 0, 31, 0xff, 0xe1, 0, 3, 0x67, 0x64, 0x00, 1, 0, 2, 0x68, 0xee,
    };
    static const uint8_t raw_avcc[] = {
        1, 100, 0, 31, 0xff, 0xe1, 0, 3, 0x67, 0x64, 0x00, 1, 0, 2, 0x68, 0xee,
    };
    static const uint8_t expected_config[] = {
        0, 0, 0, 1, 0x67, 0x64, 0x00,
        0, 0, 0, 1, 0x68, 0xee,
    };
    callback_capture_t capture = {0};
    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES];
    uint8_t body_storage[AIRPLAY_SCREEN_CONFIG_BYTES + sizeof(avcc) + AIRPLAY_SCREEN_TAG_BYTES];
    uint8_t *body = body_storage + AIRPLAY_SCREEN_CONFIG_BYTES;
    uint8_t extracted_config[32];
    size_t extracted_config_len = 0U;

    assert(airplay_screen_crypto_self_test());
    assert(airplay_screen_extract_avcc_to_annex_b(raw_avcc, sizeof(raw_avcc),
                                                  extracted_config,
                                                  sizeof(extracted_config),
                                                  &extracted_config_len));
    assert(extracted_config_len == sizeof(expected_config));
    assert(memcmp(extracted_config, expected_config, sizeof(expected_config)) == 0);
    static const uint8_t hvc_config[] = {
        0, 0, 0, 0, 'h', 'v', 'c', 'C', 1, 1, 0, 0, 0, 0,
    };
    assert(!airplay_screen_extract_avcc_to_annex_b(hvc_config, sizeof(hvc_config),
                                                   extracted_config,
                                                   sizeof(extracted_config),
                                                   &extracted_config_len));

    airplay_screen_stream_t *stream = airplay_screen_stream_create(
        key, sizeof(body_storage) - AIRPLAY_SCREEN_CONFIG_BYTES,
        capture_message, &capture);
    assert(stream != NULL);

    make_config_header(sizeof(avcc), header);
    uint8_t avcc_body[sizeof(avcc)];
    memcpy(avcc_body, avcc, sizeof(avcc));
    assert(airplay_screen_stream_process(stream, header, avcc_body,
                                         sizeof(avcc_body), 0U, 11111U) == ESP_OK);
    assert(capture.calls == 1U && capture.type == AIRPLAY_SCREEN_MESSAGE_CONFIG_H264);
    assert(capture.len == sizeof(expected_config));
    assert(memcmp(capture.bytes, expected_config, sizeof(expected_config)) == 0);
    assert(s_media_capture.config_calls == 1U && s_media_capture.frame_calls == 0U);
    assert(s_media_capture.config_len == sizeof(expected_config));
    assert(memcmp(s_media_capture.config, expected_config, sizeof(expected_config)) == 0);

    size_t body_len = encrypt_frame(key, 0U, plaintext, sizeof(plaintext),
                                    header, body);
    assert(airplay_screen_stream_process(stream, header, body, body_len,
                                         AIRPLAY_SCREEN_CONFIG_BYTES, 12345U) == ESP_OK);
    assert(airplay_screen_stream_frame_counter(stream) == 1U);
    assert(capture.calls == 2U);
    assert(capture.type == AIRPLAY_SCREEN_MESSAGE_FRAME_H264);
    assert(capture.prefix_capacity == AIRPLAY_SCREEN_CONFIG_BYTES);
    assert(capture.len == sizeof(expected_annex_b));
    assert(memcmp(capture.bytes, expected_annex_b, sizeof(expected_annex_b)) == 0);
    assert(capture.sender_time_ns == UINT64_C(2500000000));
    assert(capture.arrival_time_us == 12345U);
    assert(s_media_capture.frame_calls == 1U);
    assert(s_media_capture.frame_pointer == body);
    assert(s_media_capture.prefix_capacity == AIRPLAY_SCREEN_CONFIG_BYTES);
    assert(s_media_capture.frame_len == sizeof(expected_annex_b));
    assert(memcmp(s_media_capture.frame, expected_annex_b, sizeof(expected_annex_b)) == 0);
    assert(s_media_capture.sender_time_ns == UINT64_C(2500000000));
    assert(s_media_capture.arrival_time_us == 12345U);
    assert(memcmp(body, expected_annex_b, sizeof(expected_annex_b)) == 0);

    body_len = encrypt_frame(key, 1U, plaintext, sizeof(plaintext), header, body);
    body[body_len - 1U] ^= 0x80U;
    assert(airplay_screen_stream_process(stream, header, body, body_len,
                                         AIRPLAY_SCREEN_CONFIG_BYTES, 23456U) ==
           ESP_ERR_INVALID_MAC);
    assert(airplay_screen_stream_auth_failures(stream) == 1U);
    assert(airplay_screen_stream_frame_counter(stream) == 1U);
    assert(capture.calls == 2U);
    assert(s_media_capture.frame_calls == 1U);

    /* Authentication failure does not advance the frame counter. */
    body_len = encrypt_frame(key, 1U, plaintext, sizeof(plaintext), header, body);
    assert(airplay_screen_stream_process(stream, header, body, body_len,
                                         AIRPLAY_SCREEN_CONFIG_BYTES, 34567U) == ESP_OK);
    assert(airplay_screen_stream_frame_counter(stream) == 2U);
    assert(capture.calls == 3U);
    assert(s_media_capture.frame_calls == 2U);
    assert(s_media_capture.arrival_time_us == 34567U);

    airplay_screen_stream_destroy(stream);

    const size_t large_plaintext_len = 600U * 1024U;
    uint8_t *large_body_storage = malloc(AIRPLAY_SCREEN_CONFIG_BYTES + large_plaintext_len +
                                         AIRPLAY_SCREEN_TAG_BYTES);
    assert(large_body_storage != NULL);
    uint8_t *large_body = large_body_storage + AIRPLAY_SCREEN_CONFIG_BYTES;
    put_be32(large_body, (uint32_t)(large_plaintext_len - 4U));
    large_body[4] = 0x65U;
    memset(large_body + 5U, 0x55, large_plaintext_len - 5U);
    uint32_t large_frame_calls = 0U;
    airplay_screen_stream_t *large_stream = airplay_screen_stream_create(
        key, AIRPLAY_SCREEN_SERVER_MAX_BODY_BYTES, discard_large_message,
        &large_frame_calls);
    assert(large_stream != NULL);
    body_len = encrypt_frame(key, 0U, large_body, large_plaintext_len,
                             header, large_body);
    assert(body_len <= AIRPLAY_SCREEN_SERVER_MAX_BODY_BYTES);
    assert(airplay_screen_stream_process(large_stream, header, large_body,
                                         body_len, AIRPLAY_SCREEN_CONFIG_BYTES,
                                         45678U) == ESP_OK);
    assert(large_frame_calls == 1U);
    assert(airplay_screen_stream_frame_counter(large_stream) == 1U);
    static const uint8_t annex_b_start_code[] = {0U, 0U, 0U, 1U};
    assert(memcmp(large_body, annex_b_start_code, sizeof(annex_b_start_code)) == 0);
    airplay_screen_stream_destroy(large_stream);
    free(large_body_storage);

    puts("airplay screen stream host test passed");
    return 0;
}
