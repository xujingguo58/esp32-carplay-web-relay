/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_screen_server.h"

#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "airplay_screen_lifecycle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net_port.h"
#include "time_port.h"

#define AIRPLAY_SCREEN_SERVER_TASK_STACK 8192U
#define AIRPLAY_SCREEN_SERVER_TASK_PRIORITY 5U
#define AIRPLAY_SCREEN_SOCKET_TIMEOUT_MS 250U
#define AIRPLAY_SCREEN_RX_CHUNK_BYTES 1024U

static const char *TAG = "airplay_screen";
static portMUX_TYPE s_server_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    int listener_fd;
    int client_fd;
    bool stop_requested;
    uint16_t bound_port;
    size_t max_body_bytes;
    uint8_t *rx_body;
    size_t rx_body_capacity;
    uint32_t rx_body_caps;
    size_t max_body_observed;
    airplay_screen_stream_t *stream;
    void *context;
    airplay_screen_server_exit_cb_t on_unexpected_exit;
    void *exit_context;
    SemaphoreHandle_t stopped;
} airplay_screen_server_t;

static airplay_screen_server_t *s_server;

static bool reserve_rx_body(airplay_screen_server_t *server, size_t required)
{
    if (required <= server->rx_body_capacity) return true;

    size_t allocation_size = server->rx_body_capacity;
    if (allocation_size == 0U) {
        allocation_size = server->max_body_bytes < AIRPLAY_SCREEN_RX_CHUNK_BYTES
                              ? server->max_body_bytes
                              : AIRPLAY_SCREEN_RX_CHUNK_BYTES;
    }
    while (allocation_size < required) {
        if (allocation_size >= server->max_body_bytes / 2U) {
            allocation_size = server->max_body_bytes;
        } else {
            allocation_size *= 2U;
        }
    }

    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void *buffer = NULL;
    uint32_t caps = server->rx_body_caps;

    if (server->rx_body != NULL && caps == psram_caps) {
        buffer = heap_caps_realloc(server->rx_body, allocation_size, psram_caps);
        caps = psram_caps;
    }
    if (buffer == NULL && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0U) {
        buffer = heap_caps_malloc(allocation_size, psram_caps);
        if (buffer != NULL) {
            if (server->rx_body != NULL) {
                memset(server->rx_body, 0, server->rx_body_capacity);
                heap_caps_free(server->rx_body);
            }
            caps = psram_caps;
        }
    }
    if (buffer == NULL && allocation_size <= 64U * 1024U) {
        if (server->rx_body != NULL && server->rx_body_caps == internal_caps) {
            buffer = heap_caps_realloc(server->rx_body, allocation_size, internal_caps);
        } else {
            buffer = heap_caps_malloc(allocation_size, internal_caps);
            if (buffer != NULL && server->rx_body != NULL) {
                memset(server->rx_body, 0, server->rx_body_capacity);
                heap_caps_free(server->rx_body);
            }
        }
        caps = internal_caps;
    }

    if (buffer == NULL) return false;
    server->rx_body = buffer;
    server->rx_body_capacity = allocation_size;
    server->rx_body_caps = caps;
    return true;
}

static void release_rx_body(airplay_screen_server_t *server)
{
    if (server->rx_body != NULL) {
        memset(server->rx_body, 0, server->rx_body_capacity);
        heap_caps_free(server->rx_body);
        server->rx_body = NULL;
        server->rx_body_capacity = 0U;
        server->rx_body_caps = 0U;
    }
}

static bool is_stopping(airplay_screen_server_t *server)
{
    bool stopping;
    portENTER_CRITICAL(&s_server_mux);
    stopping = server->stop_requested;
    portEXIT_CRITICAL(&s_server_mux);
    return stopping;
}

static bool read_fully(airplay_screen_server_t *server, int fd, uint8_t *buffer, size_t size)
{
    size_t offset = 0;
    while (offset < size && !is_stopping(server)) {
        ssize_t received = recv(fd, buffer + offset, size - offset, 0);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received == 0) return false;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return false;
    }
    return offset == size;
}

static bool read_screen_body(airplay_screen_server_t *server, int fd, size_t size)
{
    size_t offset = 0U;
    while (offset < size && !is_stopping(server)) {
        size_t request = size - offset;
        if (request > AIRPLAY_SCREEN_RX_CHUNK_BYTES) {
            request = AIRPLAY_SCREEN_RX_CHUNK_BYTES;
        }
        if (!reserve_rx_body(server, offset + request)) {
            ESP_LOGW(TAG, "screen_buffer=unavailable kind=receive requested=%u"
                          " capacity=%u free_internal=%u largest_internal=%u"
                          " free_psram=%u stack_hwm=%u",
                     (unsigned)(offset + request),
                     (unsigned)server->rx_body_capacity,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
            return false;
        }
        ssize_t received = recv(fd, server->rx_body + offset, request, 0);
        if (received > 0) {
            offset += (size_t)received;
            continue;
        }
        if (received == 0) return false;
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        return false;
    }
    return offset == size;
}

static void handle_client(airplay_screen_server_t *server, int fd)
{
    uint8_t header[AIRPLAY_SCREEN_HEADER_BYTES];
    while (!is_stopping(server)) {
        if (!read_fully(server, fd, header, sizeof(header))) break;
        uint32_t body_len = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                            ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
        airplay_screen_header_info_t info;
        if (body_len > server->max_body_bytes ||
            !airplay_screen_parse_header(header, body_len, &info)) {
            ESP_LOGW(TAG, "screen_message=rejected body_bytes=%" PRIu32, body_len);
            break;
        }
        size_t old_rx_capacity = server->rx_body_capacity;
        if (body_len != 0U && !read_screen_body(server, fd, body_len)) break;
        uint64_t frame_counter_before =
            airplay_screen_stream_frame_counter(server->stream);
        esp_err_t err = airplay_screen_stream_process(server->stream, header,
                                                      server->rx_body, body_len,
                                                      (uint64_t)time_port_monotonic_us());
        uint64_t frame_counter_after =
            airplay_screen_stream_frame_counter(server->stream);
        if (body_len > server->max_body_observed ||
            server->rx_body_capacity != old_rx_capacity) {
            if (body_len > server->max_body_observed) {
                server->max_body_observed = body_len;
            }
            const char *rx_memory =
                server->rx_body_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
                    ? "psram" : "internal";
            ESP_LOGI(TAG, "screen_buffers=high_water body=%u rx=%u rx_memory=%s"
                          " decrypt=in_place free_internal=%u largest_internal=%u"
                          " free_psram=%u stack_hwm=%u",
                     (unsigned)server->max_body_observed,
                     (unsigned)server->rx_body_capacity,
                     rx_memory,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                     (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "screen_message=discarded opcode=%u error=%s auth_failures=%" PRIu32,
                     info.opcode, esp_err_to_name(err),
                     airplay_screen_stream_auth_failures(server->stream));
            if (err == ESP_ERR_INVALID_MAC ||
                (err == ESP_ERR_NO_MEM && info.opcode == 0U &&
                 frame_counter_after == frame_counter_before)) {
                ESP_LOGW(TAG, "screen_peer=closing reason=%s",
                         err == ESP_ERR_INVALID_MAC ? "frame_authentication_failed"
                                                    : "receive_buffer_unavailable");
                break;
            }
        }
    }
}

static void screen_server_task(void *argument)
{
    airplay_screen_server_t *server = argument;
    bool peer_accepted = false;
    while (airplay_screen_lifecycle_may_accept(peer_accepted,
                                                is_stopping(server))) {
        struct sockaddr_in remote = {0};
        socklen_t remote_len = sizeof(remote);
        int client = accept(server->listener_fd, (struct sockaddr *)&remote, &remote_len);
        if (client < 0) {
            if (is_stopping(server)) break;
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            ESP_LOGW(TAG, "screen_accept=failed errno=%d", errno);
            break;
        }
        peer_accepted = true;
        struct timeval timeout = {
            .tv_sec = 0,
            .tv_usec = AIRPLAY_SCREEN_SOCKET_TIMEOUT_MS * 1000,
        };
        (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        portENTER_CRITICAL(&s_server_mux);
        server->client_fd = client;
        portEXIT_CRITICAL(&s_server_mux);
        ESP_LOGI(TAG, "screen_peer=connected port=%u", server->bound_port);
        handle_client(server, client);
        portENTER_CRITICAL(&s_server_mux);
        server->client_fd = -1;
        portEXIT_CRITICAL(&s_server_mux);
        shutdown(client, SHUT_RDWR);
        close(client);
        if (!is_stopping(server)) {
            ESP_LOGI(TAG, "screen_peer=disconnected key_reuse=blocked");
        }
    }
    portENTER_CRITICAL(&s_server_mux);
    int listener = server->listener_fd;
    server->listener_fd = -1;
    portEXIT_CRITICAL(&s_server_mux);
    if (listener >= 0) {
        shutdown(listener, SHUT_RDWR);
        close(listener);
    }
    if (airplay_screen_lifecycle_is_unexpected_exit(is_stopping(server)) &&
        server->on_unexpected_exit != NULL) {
        server->on_unexpected_exit(server->exit_context);
    }
    xSemaphoreGive(server->stopped);
    vTaskDelete(NULL);
}

esp_err_t airplay_screen_server_start(uint16_t requested_port,
                                      const uint8_t output_key[AIRPLAY_SCREEN_KEY_BYTES],
                                      size_t max_body_bytes,
                                      airplay_screen_message_cb_t callback,
                                      void *context,
                                      airplay_screen_server_exit_cb_t on_unexpected_exit,
                                      void *exit_context,
                                      uint16_t *out_port)
{
    if (output_key == NULL || callback == NULL || out_port == NULL || max_body_bytes == 0 ||
        max_body_bytes > AIRPLAY_SCREEN_SERVER_MAX_BODY_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_server_mux);
    bool already_started = s_server != NULL;
    portEXIT_CRITICAL(&s_server_mux);
    if (already_started) return ESP_ERR_INVALID_STATE;

    airplay_screen_server_t *server = calloc(1, sizeof(*server));
    if (server == NULL) return ESP_ERR_NO_MEM;
    server->listener_fd = -1;
    server->client_fd = -1;
    server->max_body_bytes = max_body_bytes;
    server->context = context;
    server->on_unexpected_exit = on_unexpected_exit;
    server->exit_context = exit_context;
    server->stream = airplay_screen_stream_create(output_key, max_body_bytes,
                                                  callback, context);
    if (server->stream == NULL) {
        free(server);
        return ESP_ERR_NO_MEM;
    }
    int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listener < 0) {
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_FAIL;
    }
    int reuse = 1;
    (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    esp_netif_t *ap_netif = net_port_get_ap_netif();
    esp_netif_ip_info_t ap_ip_info;
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ap_ip_info) != ESP_OK ||
        ap_ip_info.ip.addr == 0U) {
        close(listener);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_ERR_INVALID_STATE;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(requested_port),
        .sin_addr.s_addr = ap_ip_info.ip.addr,
    };
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(listener, 1) != 0) {
        close(listener);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(listener, (struct sockaddr *)&address, &address_len) != 0) {
        close(listener);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_FAIL;
    }
    server->listener_fd = listener;
    server->bound_port = ntohs(address.sin_port);
    server->stopped = xSemaphoreCreateBinary();
    if (server->stopped == NULL) {
        close(listener);
        heap_caps_free(server->rx_body);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_server_mux);
    s_server = server;
    portEXIT_CRITICAL(&s_server_mux);
    if (xTaskCreate(screen_server_task, "airplay_screen", AIRPLAY_SCREEN_SERVER_TASK_STACK,
                    server, AIRPLAY_SCREEN_SERVER_TASK_PRIORITY, NULL) != pdPASS) {
        portENTER_CRITICAL(&s_server_mux);
        s_server = NULL;
        portEXIT_CRITICAL(&s_server_mux);
        vSemaphoreDelete(server->stopped);
        close(listener);
        airplay_screen_stream_destroy(server->stream);
        free(server);
        return ESP_ERR_NO_MEM;
    }
    *out_port = server->bound_port;
    ESP_LOGI(TAG, "screen_listener=ready port=%u max_body=%u allocation=on_demand"
                  " free_internal=%u free_psram=%u",
             server->bound_port, (unsigned)max_body_bytes,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

void airplay_screen_server_stop(void)
{
    portENTER_CRITICAL(&s_server_mux);
    airplay_screen_server_t *server = s_server;
    if (server != NULL) server->stop_requested = true;
    int listener = server != NULL ? server->listener_fd : -1;
    int client = server != NULL ? server->client_fd : -1;
    portEXIT_CRITICAL(&s_server_mux);
    if (server == NULL) return;
    if (listener >= 0) shutdown(listener, SHUT_RDWR);
    if (client >= 0) shutdown(client, SHUT_RDWR);
    (void)xSemaphoreTake(server->stopped, portMAX_DELAY);
    portENTER_CRITICAL(&s_server_mux);
    if (s_server == server) s_server = NULL;
    portEXIT_CRITICAL(&s_server_mux);
    release_rx_body(server);
    airplay_screen_stream_destroy(server->stream);
    vSemaphoreDelete(server->stopped);
    free(server);
}
