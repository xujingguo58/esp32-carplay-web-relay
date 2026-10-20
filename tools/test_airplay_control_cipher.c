/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_control_cipher.h"
#include "airplay_control_port.h"
#include "airplay_bplist.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct test_mutex {
    bool locked;
};

static struct test_mutex s_test_mutex;
static TickType_t s_test_tick;
static uint8_t s_event_wire[AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES +
                            AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES];
static size_t s_event_wire_len;
static unsigned s_event_write_calls;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    s_test_mutex.locked = false;
    return &s_test_mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    if (semaphore == NULL || semaphore->locked) return pdFALSE;
    semaphore->locked = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL || !semaphore->locked) return pdFALSE;
    semaphore->locked = false;
    return pdTRUE;
}

TickType_t xTaskGetTickCount(void)
{
    return s_test_tick;
}

static bool capture_event_write(const uint8_t *bytes, size_t len, void *context)
{
    (void)context;
    assert(bytes != NULL && len <= sizeof(s_event_wire));
    memcpy(s_event_wire, bytes, len);
    s_event_wire_len = len;
    ++s_event_write_calls;
    return true;
}

static void assert_force_keyframe_request(airplay_control_cipher_t *client,
                                          uint32_t expected_cseq)
{
    uint8_t plaintext[AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + 1U];
    size_t consumed = 0U;
    size_t plaintext_len = 0U;
    assert(airplay_control_cipher_decrypt(
               client, s_event_wire, s_event_wire_len, &consumed,
               plaintext, sizeof(plaintext) - 1U, &plaintext_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == s_event_wire_len && plaintext_len > 0U);
    plaintext[plaintext_len] = '\0';
    assert(strstr((char *)plaintext, "POST /command RTSP/1.0\r\n") ==
           (char *)plaintext);
    assert(strstr((char *)plaintext, "Content-Length: 65\r\n") != NULL);
    char cseq[32];
    snprintf(cseq, sizeof(cseq), "CSeq: %u\r\n", (unsigned)expected_cseq);
    assert(strstr((char *)plaintext, cseq) != NULL);
    assert(strstr((char *)plaintext, "X-Apple-StreamID:") == NULL);

    char *body = strstr((char *)plaintext, "\r\n\r\n");
    assert(body != NULL);
    body += 4;
    size_t body_offset = (size_t)(body - (char *)plaintext);
    airplay_bplist_doc_t document;
    airplay_bplist_object_t root;
    airplay_bplist_object_t type;
    char type_text[32];
    size_t type_len = 0U;
    assert(airplay_bplist_open((const uint8_t *)body,
                               plaintext_len - body_offset, &document) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_root(&document, &root) == AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_dict_get(&document, root, "type", &type) ==
           AIRPLAY_BPLIST_OK);
    assert(airplay_bplist_get_string(&document, type, type_text,
                                     sizeof(type_text), &type_len) ==
           AIRPLAY_BPLIST_OK);
    assert(type_len == sizeof("forceKeyFrame") - 1U);
    assert(strcmp(type_text, "forceKeyFrame") == 0);
}

static void create_pair(const uint8_t client_read[32],
                        const uint8_t client_write[32],
                        airplay_control_cipher_t **client,
                        airplay_control_cipher_t **server)
{
    assert(airplay_control_cipher_create(client_read, client_write, client) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_create(client_write, client_read, server) ==
           AIRPLAY_CONTROL_CIPHER_OK);
}

static void test_fragmented_and_coalesced_frames(void)
{
    uint8_t client_read[32];
    uint8_t client_write[32];
    for (size_t i = 0; i < sizeof(client_read); ++i) {
        client_read[i] = (uint8_t)(0x20U + i);
        client_write[i] = (uint8_t)(0x80U + i);
    }
    airplay_control_cipher_t *client = NULL;
    airplay_control_cipher_t *server = NULL;
    create_pair(client_read, client_write, &client, &server);

    static const uint8_t first[] = "POST /command RTSP/1.0\r\nCSeq: 1\r\n\r\n";
    static const uint8_t second[] = "POST /command RTSP/1.0\r\nCSeq: 2\r\n\r\n";
    uint8_t wire1[128];
    uint8_t wire2[128];
    size_t wire1_len = 0U;
    size_t wire2_len = 0U;
    assert(airplay_control_cipher_encrypt(client, first, sizeof(first) - 1U,
                                          wire1, sizeof(wire1), &wire1_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(airplay_control_cipher_encrypt(client, second, sizeof(second) - 1U,
                                          wire2, sizeof(wire2), &wire2_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);

    uint8_t combined[256];
    memcpy(combined, wire1, wire1_len);
    memcpy(combined + wire1_len, wire2, wire2_len);
    uint8_t plaintext[256];
    size_t plain_len = 0U;
    for (size_t i = 0; i < wire1_len + wire2_len; ++i) {
        size_t consumed = 0U;
        size_t produced = 0U;
        airplay_control_cipher_result_t result = airplay_control_cipher_decrypt(
            server, combined + i, 1U, &consumed,
            plaintext + plain_len, sizeof(plaintext) - plain_len, &produced);
        assert(consumed == 1U);
        assert(result == (i + 1U == wire1_len + wire2_len
                              ? AIRPLAY_CONTROL_CIPHER_OK
                              : AIRPLAY_CONTROL_CIPHER_INCOMPLETE) ||
               result == AIRPLAY_CONTROL_CIPHER_OK);
        plain_len += produced;
    }
    assert(plain_len == sizeof(first) - 1U + sizeof(second) - 1U);
    assert(memcmp(plaintext, first, sizeof(first) - 1U) == 0);
    assert(memcmp(plaintext + sizeof(first) - 1U, second,
                  sizeof(second) - 1U) == 0);

    uint8_t reply_wire[128];
    size_t reply_wire_len = 0U;
    assert(airplay_control_cipher_encrypt(server, second, sizeof(second) - 1U,
                                          reply_wire, sizeof(reply_wire),
                                          &reply_wire_len) == AIRPLAY_CONTROL_CIPHER_OK);
    size_t consumed = 0U;
    size_t produced = 0U;
    assert(airplay_control_cipher_decrypt(client, reply_wire, reply_wire_len,
                                          &consumed, plaintext, sizeof(plaintext),
                                          &produced) == AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == reply_wire_len && produced == sizeof(second) - 1U);
    assert(memcmp(plaintext, second, produced) == 0);

    airplay_control_cipher_destroy(client);
    airplay_control_cipher_destroy(server);
}

static void test_output_capacity_and_empty_frame(void)
{
    uint8_t read_key[32] = {1U};
    uint8_t write_key[32] = {2U};
    airplay_control_cipher_t *client = NULL;
    airplay_control_cipher_t *server = NULL;
    create_pair(read_key, write_key, &client, &server);

    static const uint8_t message[] = "touch report";
    uint8_t wire[64];
    size_t wire_len = 0U;
    assert(airplay_control_cipher_encrypt(client, message, sizeof(message) - 1U,
                                          wire, sizeof(wire), &wire_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    size_t consumed = 0U;
    size_t produced = 0U;
    uint8_t small[4];
    assert(airplay_control_cipher_decrypt(server, wire, wire_len, &consumed,
                                          small, sizeof(small), &produced) ==
           AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL);
    assert(consumed == wire_len && produced == 0U);
    uint8_t plaintext[64];
    assert(airplay_control_cipher_decrypt(server, NULL, 0U, &consumed,
                                          plaintext, sizeof(plaintext), &produced) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == 0U && produced == sizeof(message) - 1U);
    assert(memcmp(plaintext, message, produced) == 0);

    assert(airplay_control_cipher_encrypt(client, NULL, 0U, wire, sizeof(wire),
                                          &wire_len) == AIRPLAY_CONTROL_CIPHER_OK);
    assert(wire_len == AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES);
    assert(airplay_control_cipher_decrypt(server, wire, wire_len, &consumed,
                                          NULL, 0U, &produced) == AIRPLAY_CONTROL_CIPHER_OK);
    assert(consumed == wire_len && produced == 0U);

    airplay_control_cipher_destroy(client);
    airplay_control_cipher_destroy(server);
}

static void test_bad_tag_and_oversized_length_poison_session(void)
{
    uint8_t read_key[32] = {3U};
    uint8_t write_key[32] = {4U};
    airplay_control_cipher_t *client = NULL;
    airplay_control_cipher_t *server = NULL;
    create_pair(read_key, write_key, &client, &server);
    static const uint8_t message[] = "authenticated";
    uint8_t wire[64];
    size_t wire_len = 0U;
    assert(airplay_control_cipher_encrypt(client, message, sizeof(message) - 1U,
                                          wire, sizeof(wire), &wire_len) ==
           AIRPLAY_CONTROL_CIPHER_OK);
    wire[wire_len - 1U] ^= 0x80U;
    size_t consumed = 0U;
    size_t produced = 0U;
    uint8_t plaintext[64];
    assert(airplay_control_cipher_decrypt(server, wire, wire_len, &consumed,
                                          plaintext, sizeof(plaintext), &produced) ==
           AIRPLAY_CONTROL_CIPHER_AUTH_FAILED);
    assert(airplay_control_cipher_decrypt(server, NULL, 0U, &consumed,
                                          plaintext, sizeof(plaintext), &produced) ==
           AIRPLAY_CONTROL_CIPHER_FAILED_STATE);
    airplay_control_cipher_destroy(client);
    airplay_control_cipher_destroy(server);

    create_pair(read_key, write_key, &client, &server);
    const uint8_t oversized_header[] = {0x01U, 0x20U}; /* 8193 bytes */
    assert(airplay_control_cipher_decrypt(server, oversized_header,
                                          sizeof(oversized_header), &consumed,
                                          plaintext, sizeof(plaintext), &produced) ==
           AIRPLAY_CONTROL_CIPHER_FRAME_TOO_LARGE);
    assert(consumed == sizeof(oversized_header));
    airplay_control_cipher_destroy(client);
    airplay_control_cipher_destroy(server);
}

static void test_encrypted_force_keyframe_command(void)
{
    uint8_t server_read_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES];
    uint8_t server_write_key[AIRPLAY_CONTROL_CIPHER_KEY_BYTES];
    for (size_t i = 0U; i < sizeof(server_read_key); ++i) {
        server_read_key[i] = (uint8_t)(0x30U + i);
        server_write_key[i] = (uint8_t)(0xa0U + i);
    }
    airplay_control_cipher_t *client = NULL;
    airplay_control_cipher_t *unused_server = NULL;
    create_pair(server_write_key, server_read_key, &client, &unused_server);
    airplay_control_cipher_destroy(unused_server);

    assert(airplay_control_port_init() == ESP_OK);
    assert(airplay_control_port_attach(server_read_key, server_write_key,
                                       77U, 40U, capture_event_write, NULL) == ESP_OK);
    s_test_tick = 1000U;
    s_event_write_calls = 0U;
    assert(airplay_control_port_request_main_screen_keyframe());
    assert(s_event_write_calls == 1U);
    assert_force_keyframe_request(client, 41U);

    /* A burst of PLI events coalesces to the already-sent recovery request. */
    assert(airplay_control_port_request_main_screen_keyframe());
    assert(s_event_write_calls == 1U);

    s_test_tick += pdMS_TO_TICKS(251U);
    assert(airplay_control_port_request_main_screen_keyframe());
    assert(s_event_write_calls == 2U);
    assert_force_keyframe_request(client, 42U);

    airplay_control_port_detach();
    assert(!airplay_control_port_request_main_screen_keyframe());
    airplay_control_cipher_destroy(client);
}

int main(void)
{
    test_fragmented_and_coalesced_frames();
    test_output_capacity_and_empty_frame();
    test_bad_tag_and_oversized_length_poison_session();
    test_encrypted_force_keyframe_command();
    puts("airplay ControlCipher host test passed");
    return 0;
}
