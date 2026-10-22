/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Single-client RTSP socket lifecycle for the ESP-IDF AirPlay session router.
 * AirPlay service discovery and authorization material are provided outside
 * this component; it never fabricates an authenticated session.
 */
#include "airplay_rtsp_server.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "airplay_bplist.h"
#include "airplay_control_cipher.h"
#include "airplay_control_port.h"
#include "airplay_event_port.h"
#include "airplay_event_lifecycle.h"
#include "airplay_screen_lifecycle.h"
#include "airplay_media_bridge.h"
#include "airplay_timing_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mbedtls/platform_util.h"
#include "net_port.h"

#define RTSP_SERVER_TASK_STACK_BYTES 10240U
#define RTSP_SERVER_TASK_PRIORITY 5U
#define RTSP_SERVER_POLL_TIMEOUT_MS 250U
#define RTSP_SERVER_RX_CHUNK_BYTES 1200U
#define RTSP_SERVER_STREAM_BUFFER_BYTES 6144U
#define RTSP_SERVER_RESPONSE_BYTES AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES
#define RTSP_SERVER_CIPHER_FRAME_BYTES \
    (AIRPLAY_CONTROL_CIPHER_MAX_FRAME_BYTES + AIRPLAY_CONTROL_CIPHER_OVERHEAD_BYTES)

static const char *TAG = "airplay_rtsp";

typedef struct {
    uint8_t stream[RTSP_SERVER_STREAM_BUFFER_BYTES];
    uint8_t response[RTSP_SERVER_RESPONSE_BYTES];
    uint8_t io[RTSP_SERVER_CIPHER_FRAME_BYTES];
    airplay_rtsp_request_t request;
    airplay_control_cipher_t *cipher;
    size_t stream_len;
} rtsp_connection_t;

struct airplay_rtsp_server {
    airplay_rtsp_server_config_t config;
    uint8_t info_plist[AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES];
    int listener_fd;
    int client_fd;
    uint16_t port;
    volatile bool stop_requested;
    volatile bool event_connected;
    volatile bool event_key_used;
    volatile bool event_failed;
    volatile bool event_stopping;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
    airplay_event_port_t *event_port;
    airplay_timing_port_t *timing_port;
};

static portMUX_TYPE s_server_mux = portMUX_INITIALIZER_UNLOCKED;
static airplay_rtsp_server_t *s_server;

static void *server_calloc(size_t count, size_t size)
{
    if (count == 0U || size == 0U || count > SIZE_MAX / size) return NULL;
    size_t bytes = count * size;
    void *buffer = heap_caps_calloc(count, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        buffer = heap_caps_calloc(count, size,
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (buffer == NULL) return NULL;
    memset(buffer, 0, bytes);
    return buffer;
}

static void event_state_changed(bool connected, void *context);
static void screen_stream_exited(void *context);
static void stop_event(void *context);
static void stop_timing(void *context);

static void server_free(void *buffer, size_t size)
{
    if (buffer == NULL) return;
    mbedtls_platform_zeroize(buffer, size);
    heap_caps_free(buffer);
}

static bool socket_write_all(int fd, const uint8_t *bytes, size_t len)
{
    size_t offset = 0U;
    while (offset < len) {
        int sent = send(fd, bytes + offset, len - offset, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

static bool start_screen(const airplay_rtsp_pairing_t *pairing,
                         uint64_t stream_connection_id,
                         uint16_t requested_port,
                         uint16_t *bound_port,
                         void *context)
{
    airplay_rtsp_server_t *server = context;
    return airplay_media_bridge_start_screen_stream(
               pairing, stream_connection_id, requested_port,
               screen_stream_exited, server, bound_port) == ESP_OK;
}

static void screen_stream_exited(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    int rtsp_fd = -1;
    bool unexpected_exit;
    portENTER_CRITICAL(&s_server_mux);
    unexpected_exit = airplay_screen_lifecycle_is_unexpected_exit(
        server->stop_requested);
    if (unexpected_exit) rtsp_fd = server->client_fd;
    portEXIT_CRITICAL(&s_server_mux);
    if (!unexpected_exit) return;
    ESP_LOGW(TAG, "screen_stream=lost rtsp_session=closing key_reuse=blocked");
    if (rtsp_fd >= 0) shutdown(rtsp_fd, SHUT_RDWR);
}

static bool start_event(const airplay_rtsp_pairing_t *pairing,
                        uint16_t *bound_port,
                        void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL) return false;
    portENTER_CRITICAL(&s_server_mux);
    bool event_failed = server->event_failed;
    bool event_key_used = server->event_key_used;
    bool listener_active = server->event_port != NULL;
    bool may_start = airplay_event_lifecycle_may_start(
        listener_active, event_key_used, event_failed);
    if (listener_active) {
        uint16_t existing_port = airplay_event_port_get_port(server->event_port);
        portEXIT_CRITICAL(&s_server_mux);
        if (!may_start) return false;
        *bound_port = existing_port;
        return *bound_port != 0U;
    }
    portEXIT_CRITICAL(&s_server_mux);
    /* Event AEAD counters restart at zero on attach. Do not reuse Pair-Verify
     * event keys after a connected listener has been stopped in this session. */
    if (!may_start) return false;
    airplay_pair_verify_event_keys_t keys;
    airplay_pair_verify_result_t key_result =
        airplay_rtsp_pairing_get_event_keys(pairing, &keys);
    if (key_result != AIRPLAY_PAIR_VERIFY_OK) {
        mbedtls_platform_zeroize(&keys, sizeof(keys));
        ESP_LOGW(TAG, "event_setup=pair_verify_not_ready result=%d", (int)key_result);
        return false;
    }
    airplay_event_port_config_t event_config = {
        .keys = keys,
        .stream_id = 0U,
        .initial_cseq = 0U,
        .on_state = event_state_changed,
        .context = server,
    };
    airplay_event_port_t *event_port = NULL;
    esp_err_t result = airplay_event_port_start(&event_config,
                                                &event_port);
    mbedtls_platform_zeroize(&keys, sizeof(keys));
    mbedtls_platform_zeroize(&event_config, sizeof(event_config));
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "event_setup=listener_failed error=%s", esp_err_to_name(result));
        return false;
    }
    portENTER_CRITICAL(&s_server_mux);
    server->event_port = event_port;
    bool event_failed_during_start = server->event_failed;
    portEXIT_CRITICAL(&s_server_mux);
    if (event_failed_during_start) {
        stop_event(server);
        return false;
    }
    *bound_port = airplay_event_port_get_port(event_port);
    if (*bound_port == 0U) {
        stop_event(server);
        return false;
    }
    ESP_LOGI(TAG, "event_setup=ready port=%u", (unsigned)*bound_port);
    return true;
}

static bool start_timing(uint16_t peer_port,
                         uint16_t *bound_port,
                         void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || bound_port == NULL || server->client_fd < 0) return false;
    if (server->timing_port != NULL) {
        *bound_port = airplay_timing_port_get_port(server->timing_port);
        return *bound_port != 0U;
    }
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    memset(&peer, 0, sizeof(peer));
    if (getpeername(server->client_fd, (struct sockaddr *)&peer, &peer_len) != 0 ||
        peer.sin_family != AF_INET || peer.sin_addr.s_addr == 0U) {
        ESP_LOGW(TAG, "timing_setup=peer_address_unavailable errno=%d", errno);
        return false;
    }
    airplay_timing_port_config_t config = {
        .peer_ipv4 = peer.sin_addr.s_addr,
        .peer_port = peer_port,
    };
    esp_err_t result = airplay_timing_port_start(&config, &server->timing_port);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "timing_setup=listener_failed error=%s", esp_err_to_name(result));
        return false;
    }
    *bound_port = airplay_timing_port_get_port(server->timing_port);
    ESP_LOGI(TAG, "timing_setup=ready port=%u peer_port=%u",
             (unsigned)*bound_port, (unsigned)peer_port);
    return *bound_port != 0U;
}

static void stop_screen(void *context)
{
    (void)context;
    airplay_media_bridge_stop_screen_stream();
}

static void event_state_changed(bool connected, void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    bool unexpected_exit = false;
    portENTER_CRITICAL(&s_server_mux);
    server->event_connected = connected;
    if (connected) {
        server->event_key_used = true;
    } else if (airplay_event_lifecycle_is_unexpected_exit(
                   connected, server->event_stopping,
                   server->stop_requested)) {
        /* The event task reports false on every unrequested exit, including
         * listener/attach failures before a successful connection. */
        server->event_failed = true;
        unexpected_exit = true;
    }
    portEXIT_CRITICAL(&s_server_mux);
    if (unexpected_exit) {
        ESP_LOGW(TAG, "event_channel=lost rtsp_session=continuing input=disabled key_reuse=blocked");
    }
    if (server->config.on_event_state != NULL) {
        server->config.on_event_state(connected,
                                      server->config.event_state_context);
    }
}

static void stop_event(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL) return;
    portENTER_CRITICAL(&s_server_mux);
    airplay_event_port_t *event_port = server->event_port;
    if (event_port != NULL) {
        server->event_port = NULL;
        server->event_connected = false;
    }
    if (event_port != NULL) {
        /* Suppress a fatal-session transition for an intentional stop. */
        server->event_stopping = true;
    }
    portEXIT_CRITICAL(&s_server_mux);
    if (event_port == NULL) return;
    airplay_event_port_stop(event_port);
    portENTER_CRITICAL(&s_server_mux);
    server->event_stopping = false;
    portEXIT_CRITICAL(&s_server_mux);
}

static void stop_timing(void *context)
{
    airplay_rtsp_server_t *server = context;
    if (server == NULL || server->timing_port == NULL) return;
    airplay_timing_port_t *timing_port = server->timing_port;
    server->timing_port = NULL;
    airplay_timing_port_stop(timing_port);
}

static airplay_rtsp_session_result_t create_session(
    airplay_rtsp_server_t *server,
    airplay_rtsp_session_t **out_session)
{
    airplay_rtsp_session_config_t config = {
        .pairing = server->config.pairing,
        .info_plist = server->info_plist,
        .info_plist_len = server->config.info_plist_len,
        .screen_requested_port = server->config.screen_port,
        .start_screen = start_screen,
        .start_event = start_event,
        .start_timing = start_timing,
        .stop_screen = stop_screen,
        .stop_event = stop_event,
        .stop_timing = stop_timing,
        .media_context = server,
    };
    return airplay_rtsp_session_create(&config, out_session);
}

static void consume_stream(rtsp_connection_t *connection, size_t count)
{
    if (count < connection->stream_len) {
        memmove(connection->stream, connection->stream + count,
                connection->stream_len - count);
    }
    connection->stream_len -= count;
}

static bool append_stream(rtsp_connection_t *connection,
                          const uint8_t *bytes,
                          size_t len)
{
    if (connection == NULL || (bytes == NULL && len != 0U) ||
        len > sizeof(connection->stream) - connection->stream_len) {
        return false;
    }
    if (len != 0U) {
        memcpy(connection->stream + connection->stream_len, bytes, len);
        connection->stream_len += len;
    }
    return true;
}

static bool send_response(rtsp_connection_t *connection,
                          int client_fd,
                          size_t response_len)
{
    if (connection->cipher == NULL) {
        return socket_write_all(client_fd, connection->response, response_len);
    }
    size_t encrypted_len = 0U;
    airplay_control_cipher_result_t result = airplay_control_cipher_encrypt(
        connection->cipher, connection->response, response_len,
        connection->io, sizeof(connection->io), &encrypted_len);
    return result == AIRPLAY_CONTROL_CIPHER_OK &&
           socket_write_all(client_fd, connection->io, encrypted_len);
}

static bool process_stream(airplay_rtsp_server_t *server,
                           rtsp_connection_t *connection,
                           airplay_rtsp_session_t *session,
                           int client_fd)
{
    while (connection->stream_len != 0U) {
        airplay_rtsp_parse_result_t parse_result = airplay_rtsp_parse_request(
            connection->stream, connection->stream_len, &connection->request);
        if (parse_result == AIRPLAY_RTSP_PARSE_INCOMPLETE) {
            return connection->stream_len < sizeof(connection->stream);
        }
        if (parse_result != AIRPLAY_RTSP_PARSE_COMPLETE) {
            ESP_LOGW(TAG, "rtsp_request=rejected parse=%d buffered=%u",
                     (int)parse_result, (unsigned)connection->stream_len);
            return false;
        }
        size_t response_len = 0U;
        bool enable_cipher = false;
        airplay_rtsp_session_result_t result = airplay_rtsp_session_handle(
            session, &connection->request, connection->response,
            sizeof(connection->response), &response_len, &enable_cipher);
        if (result != AIRPLAY_RTSP_SESSION_OK || response_len == 0U ||
            !send_response(connection, client_fd, response_len)) {
            ESP_LOGE(TAG, "rtsp_response=failed result=%d method=%s path=%s",
                     (int)result, connection->request.method,
                     connection->request.path);
            return false;
        }
        size_t consumed = connection->request.consumed;
        consume_stream(connection, consumed);
        ESP_LOGI(TAG, "rtsp_request=handled method=%s path=%s cseq=%s bytes=%u",
                 connection->request.method, connection->request.path,
                 connection->request.cseq, (unsigned)consumed);
        memset(&connection->request, 0, sizeof(connection->request));

        if (enable_cipher) {
            if (connection->stream_len != 0U) {
                ESP_LOGW(TAG, "rtsp_cipher=transition_with_pending_plaintext");
                return false;
            }
            airplay_pair_verify_control_keys_t keys;
            airplay_pair_verify_result_t key_result =
                airplay_rtsp_session_get_control_keys(session, &keys);
            if (key_result != AIRPLAY_PAIR_VERIFY_OK ||
                airplay_control_cipher_create(keys.read_key, keys.write_key,
                                              &connection->cipher) !=
                    AIRPLAY_CONTROL_CIPHER_OK) {
                mbedtls_platform_zeroize(&keys, sizeof(keys));
                ESP_LOGE(TAG, "rtsp_cipher=enable_failed result=%d",
                         (int)key_result);
                return false;
            }
            mbedtls_platform_zeroize(&keys, sizeof(keys));
            airplay_rtsp_session_mark_control_cipher_active(session);
            ESP_LOGI(TAG, "rtsp_cipher=enabled");
        }
    }
    (void)server;
    return true;
}

static bool process_received_bytes(airplay_rtsp_server_t *server,
                                  rtsp_connection_t *connection,
                                  airplay_rtsp_session_t *session,
                                  int client_fd,
                                  const uint8_t *bytes,
                                  size_t length)
{
    if (connection->cipher == NULL) {
        return append_stream(connection, bytes, length) &&
               process_stream(server, connection, session, client_fd);
    }

    size_t offset = 0U;
    bool drain_cipher = true;
    while (offset < length || drain_cipher) {
        size_t consumed = 0U;
        size_t plaintext_len = 0U;
        const uint8_t *input = offset < length ? bytes + offset : NULL;
        size_t input_len = offset < length ? length - offset : 0U;
        airplay_control_cipher_result_t result = airplay_control_cipher_decrypt(
            connection->cipher, input, input_len, &consumed,
            connection->response, sizeof(connection->response), &plaintext_len);
        offset += consumed;
        if (plaintext_len != 0U &&
            (!append_stream(connection, connection->response, plaintext_len) ||
             !process_stream(server, connection, session, client_fd))) {
            return false;
        }
        if (result == AIRPLAY_CONTROL_CIPHER_OUTPUT_TOO_SMALL) {
            drain_cipher = true;
            continue;
        }
        if (result == AIRPLAY_CONTROL_CIPHER_OK ||
            result == AIRPLAY_CONTROL_CIPHER_INCOMPLETE) {
            if (offset < length) {
                if (consumed == 0U) return false;
                drain_cipher = false;
                continue;
            }
            break;
        }
        ESP_LOGW(TAG, "rtsp_cipher=decrypt_failed result=%d", (int)result);
        return false;
    }
    return true;
}

static void serve_client(airplay_rtsp_server_t *server, int client_fd)
{
    struct timeval timeout = {.tv_sec = 0, .tv_usec = 250000};
    int no_delay = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &no_delay,
                     sizeof(no_delay));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    rtsp_connection_t *connection = server_calloc(1U, sizeof(*connection));
    airplay_rtsp_session_t *session = NULL;
    portENTER_CRITICAL(&s_server_mux);
    server->event_connected = false;
    server->event_key_used = false;
    server->event_failed = false;
    server->event_stopping = false;
    portEXIT_CRITICAL(&s_server_mux);
    if (connection == NULL || create_session(server, &session) !=
                                  AIRPLAY_RTSP_SESSION_OK) {
        ESP_LOGE(TAG, "rtsp_session=create_failed free_internal=%u free_psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        airplay_rtsp_session_destroy(session);
        server_free(connection, sizeof(*connection));
        return;
    }

    ESP_LOGI(TAG, "rtsp_client=connected free_internal=%u free_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t rx[RTSP_SERVER_RX_CHUNK_BYTES];
    while (!server->stop_requested) {
        int received = recv(client_fd, rx, sizeof(rx), 0);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (received == 0) {
            ESP_LOGI(TAG, "rtsp_client=peer_closed stage=control");
            break;
        }
        if (received < 0) {
            ESP_LOGW(TAG, "rtsp_client=receive_failed errno=%d", errno);
            break;
        }
        if (!process_received_bytes(server, connection, session, client_fd,
                                    rx, (size_t)received)) {
            break;
        }
    }
    mbedtls_platform_zeroize(rx, sizeof(rx));
    airplay_control_cipher_destroy(connection->cipher);
    airplay_rtsp_session_destroy(session);
    server_free(connection, sizeof(*connection));
    ESP_LOGI(TAG, "rtsp_client=disconnected stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

static bool wait_for_client(int listener_fd)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(listener_fd, &read_fds);
    struct timeval timeout = {
        .tv_sec = RTSP_SERVER_POLL_TIMEOUT_MS / 1000U,
        .tv_usec = (RTSP_SERVER_POLL_TIMEOUT_MS % 1000U) * 1000U,
    };
    int selected = select(listener_fd + 1, &read_fds, NULL, NULL, &timeout);
    if (selected < 0 && errno != EINTR) return false;
    return selected > 0 && FD_ISSET(listener_fd, &read_fds);
}

static void rtsp_server_task(void *argument)
{
    airplay_rtsp_server_t *server = argument;
    while (!server->stop_requested) {
        if (!wait_for_client(server->listener_fd)) continue;
        struct sockaddr_in peer;
        socklen_t peer_len = sizeof(peer);
        int client_fd = accept(server->listener_fd,
                               (struct sockaddr *)&peer, &peer_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!server->stop_requested) {
                ESP_LOGW(TAG, "rtsp_accept=failed errno=%d", errno);
            }
            continue;
        }
        portENTER_CRITICAL(&s_server_mux);
        server->client_fd = client_fd;
        portEXIT_CRITICAL(&s_server_mux);
        ESP_LOGI(TAG, "rtsp_peer=%s:%u", inet_ntoa(peer.sin_addr),
                 (unsigned)ntohs(peer.sin_port));
        serve_client(server, client_fd);
        shutdown(client_fd, SHUT_RDWR);
        close(client_fd);
        portENTER_CRITICAL(&s_server_mux);
        server->client_fd = -1;
        portEXIT_CRITICAL(&s_server_mux);
    }
    if (server->event_port != NULL) stop_event(server);
    if (server->timing_port != NULL) stop_timing(server);
    airplay_media_bridge_stop_screen_stream();
    if (server->listener_fd >= 0) {
        close(server->listener_fd);
        server->listener_fd = -1;
    }
    (void)xSemaphoreGive(server->stopped);
    vTaskDelete(NULL);
}

esp_err_t airplay_rtsp_server_start(
    const airplay_rtsp_server_config_t *config,
    airplay_rtsp_server_t **out_server)
{
    if (out_server == NULL) return ESP_ERR_INVALID_ARG;
    *out_server = NULL;
    if (config == NULL || config->info_plist == NULL ||
        config->info_plist_len == 0U ||
        config->info_plist_len > AIRPLAY_RTSP_SESSION_INFO_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    airplay_bplist_doc_t info_doc;
    airplay_bplist_object_t info_root;
    airplay_bplist_type_t info_type;
    if (airplay_bplist_open(config->info_plist, config->info_plist_len,
                            &info_doc) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_root(&info_doc, &info_root) != AIRPLAY_BPLIST_OK ||
        airplay_bplist_get_type(&info_doc, info_root, &info_type, NULL) !=
            AIRPLAY_BPLIST_OK || info_type != AIRPLAY_BPLIST_DICTIONARY) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_server_mux);
    bool already_running = s_server != NULL;
    portEXIT_CRITICAL(&s_server_mux);
    if (already_running) return ESP_ERR_INVALID_STATE;

    esp_err_t control_result = airplay_control_port_init();
    if (control_result != ESP_OK) return control_result;
    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return ESP_FAIL;
    int reuse = 1;
    (void)setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    esp_netif_t *ap_netif = net_port_get_ap_netif();
    esp_netif_ip_info_t ap_ip;
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ap_ip) != ESP_OK ||
        ap_ip.ip.addr == 0U) {
        close(listener_fd);
        return ESP_ERR_INVALID_STATE;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(config->control_port == 0U
                                 ? AIRPLAY_RTSP_DEFAULT_PORT
                                 : config->control_port);
    address.sin_addr.s_addr = ap_ip.ip.addr;
    if (bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener_fd, 1) != 0) {
        ESP_LOGE(TAG, "rtsp_bind=failed errno=%d port=%u", errno,
                 (unsigned)ntohs(address.sin_port));
        close(listener_fd);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(listener_fd, (struct sockaddr *)&address,
                    &address_len) != 0) {
        close(listener_fd);
        return ESP_FAIL;
    }

    airplay_rtsp_server_t *server = calloc(1U, sizeof(*server));
    if (server == NULL) {
        close(listener_fd);
        return ESP_ERR_NO_MEM;
    }
    server->config = *config;
    memcpy(server->info_plist, config->info_plist, config->info_plist_len);
    server->config.info_plist = server->info_plist;
    server->listener_fd = listener_fd;
    server->client_fd = -1;
    server->port = ntohs(address.sin_port);
    server->stopped = xSemaphoreCreateBinary();
    if (server->stopped == NULL) {
        close(listener_fd);
        free(server);
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_server_mux);
    if (s_server != NULL) {
        portEXIT_CRITICAL(&s_server_mux);
        close(listener_fd);
        vSemaphoreDelete(server->stopped);
        mbedtls_platform_zeroize(server, sizeof(*server));
        free(server);
        return ESP_ERR_INVALID_STATE;
    }
    s_server = server;
    portEXIT_CRITICAL(&s_server_mux);
    if (xTaskCreate(rtsp_server_task, "airplay_rtsp",
                    RTSP_SERVER_TASK_STACK_BYTES, server,
                    RTSP_SERVER_TASK_PRIORITY, &server->task) != pdPASS) {
        portENTER_CRITICAL(&s_server_mux);
        s_server = NULL;
        portEXIT_CRITICAL(&s_server_mux);
        close(listener_fd);
        vSemaphoreDelete(server->stopped);
        mbedtls_platform_zeroize(server, sizeof(*server));
        free(server);
        return ESP_ERR_NO_MEM;
    }
    *out_server = server;
    ESP_LOGI(TAG, "rtsp_listener=ready ip=" IPSTR " port=%u free_internal=%u free_psram=%u",
             IP2STR(&ap_ip.ip), (unsigned)server->port,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

uint16_t airplay_rtsp_server_get_port(const airplay_rtsp_server_t *server)
{
    return server == NULL ? 0U : server->port;
}

bool airplay_rtsp_server_is_running(void)
{
    portENTER_CRITICAL(&s_server_mux);
    bool running = s_server != NULL && !s_server->stop_requested;
    portEXIT_CRITICAL(&s_server_mux);
    return running;
}

void airplay_rtsp_server_stop(airplay_rtsp_server_t *server)
{
    if (server == NULL) return;
    portENTER_CRITICAL(&s_server_mux);
    server->stop_requested = true;
    portEXIT_CRITICAL(&s_server_mux);
    (void)xSemaphoreTake(server->stopped, portMAX_DELAY);
    portENTER_CRITICAL(&s_server_mux);
    if (s_server == server) s_server = NULL;
    portEXIT_CRITICAL(&s_server_mux);
    vSemaphoreDelete(server->stopped);
    mbedtls_platform_zeroize(server, sizeof(*server));
    free(server);
}
