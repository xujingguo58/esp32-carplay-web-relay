/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_event_port.h"

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "airplay_control_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mbedtls/platform_util.h"
#include "net_port.h"

#define EVENT_TASK_STACK_BYTES 4096U
#define EVENT_TASK_PRIORITY 5U
#define EVENT_RX_CHUNK_BYTES 1200U
#define EVENT_POLL_TIMEOUT_MS 250U

static const char *TAG = "airplay_event";

struct airplay_event_port {
    int listener_fd;
    int client_fd;
    uint16_t port;
    volatile bool stop_requested;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
    airplay_pair_verify_event_keys_t keys;
    uint32_t stream_id;
    uint32_t initial_cseq;
    airplay_event_port_state_cb_t on_state;
    void *context;
};

static bool socket_write(const uint8_t *bytes, size_t len, void *context)
{
    airplay_event_port_t *port = context;
    if (port == NULL || port->client_fd < 0 || bytes == NULL) return false;
    size_t offset = 0U;
    while (offset < len) {
        int sent = send(port->client_fd, bytes + offset, len - offset, 0);
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) return false;
        offset += (size_t)sent;
    }
    return true;
}

static bool wait_for_connection(int listener_fd, volatile bool *stop_requested)
{
    while (!*stop_requested) {
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(listener_fd, &read_fds);
        struct timeval timeout = {
            .tv_sec = EVENT_POLL_TIMEOUT_MS / 1000U,
            .tv_usec = (EVENT_POLL_TIMEOUT_MS % 1000U) * 1000U,
        };
        int result = select(listener_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return false;
        if (result == 0) continue;
        if (FD_ISSET(listener_fd, &read_fds)) return true;
    }
    return false;
}

static void event_task(void *argument)
{
    airplay_event_port_t *port = argument;
    uint8_t rx[EVENT_RX_CHUNK_BYTES];
    bool attached = false;
    bool notified_connected = false;

    if (wait_for_connection(port->listener_fd, &port->stop_requested)) {
        struct sockaddr_in peer_address;
        socklen_t peer_address_len = sizeof(peer_address);
        int client_fd = accept(port->listener_fd,
                               (struct sockaddr *)&peer_address,
                               &peer_address_len);
        if (client_fd >= 0 && !port->stop_requested) {
            port->client_fd = client_fd;
            int no_delay = 1;
            (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                             &no_delay, sizeof(no_delay));
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
            (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                             &timeout, sizeof(timeout));
            (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                             &timeout, sizeof(timeout));

            attached = airplay_control_port_attach(
                port->keys.read_key, port->keys.write_key,
                port->stream_id, port->initial_cseq,
                socket_write, port) == ESP_OK;
            mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
            if (attached) {
                ESP_LOGI(TAG, "event_connected peer=%s:%u stream_id=%" PRIu32,
                         inet_ntoa(peer_address.sin_addr),
                         (unsigned)ntohs(peer_address.sin_port), port->stream_id);
                notified_connected = true;
                if (port->on_state != NULL) port->on_state(true, port->context);
                while (!port->stop_requested) {
                    int received = recv(client_fd, rx, sizeof(rx), 0);
                    if (received < 0 && errno == EINTR) continue;
                    if (received < 0 &&
                        (errno == EAGAIN || errno == EWOULDBLOCK)) continue;
                    if (received == 0) {
                        ESP_LOGW(TAG, "event_connection=peer_closed");
                        break;
                    }
                    if (received < 0) {
                        ESP_LOGW(TAG, "event_connection=receive_failed errno=%d", errno);
                        break;
                    }
                    esp_err_t receive_result = airplay_control_port_receive_encrypted(
                        rx, (size_t)received);
                    if (receive_result != ESP_OK) {
                        ESP_LOGW(TAG, "event_rx_rejected error=%s",
                                 esp_err_to_name(receive_result));
                        break;
                    }
                }
            } else {
                ESP_LOGE(TAG, "event_control_attach_failed");
            }
        } else if (client_fd >= 0) {
            shutdown(client_fd, SHUT_RDWR);
            close(client_fd);
        }
    }

    if (attached) airplay_control_port_detach();
    /* Report connected sessions closing and unexpected listener/attach
     * failures. An intentional stop before connect has no state transition. */
    if (port->on_state != NULL &&
        (notified_connected || !port->stop_requested)) {
        port->on_state(false, port->context);
    }
    if (notified_connected) {
        ESP_LOGI(TAG, "event_disconnected stack_hwm=%u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    if (port->client_fd >= 0) {
        shutdown(port->client_fd, SHUT_RDWR);
        close(port->client_fd);
        port->client_fd = -1;
    }
    if (port->listener_fd >= 0) {
        close(port->listener_fd);
        port->listener_fd = -1;
    }
    mbedtls_platform_zeroize(rx, sizeof(rx));
    mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
    (void)xSemaphoreGive(port->stopped);
    vTaskDelete(NULL);
}

esp_err_t airplay_event_port_start(const airplay_event_port_config_t *config,
                                   airplay_event_port_t **out_port)
{
    if (out_port == NULL) return ESP_ERR_INVALID_ARG;
    *out_port = NULL;
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t control_result = airplay_control_port_init();
    if (control_result != ESP_OK) return control_result;

    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) return ESP_FAIL;
    int reuse = 1;
    (void)setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = 0;
    esp_netif_t *ap_netif = net_port_get_ap_netif();
    esp_netif_ip_info_t ap_ip_info;
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ap_ip_info) != ESP_OK ||
        ap_ip_info.ip.addr == 0U) {
        close(listener_fd);
        return ESP_ERR_INVALID_STATE;
    }
    address.sin_addr.s_addr = ap_ip_info.ip.addr;
    if (bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listener_fd, 1) != 0) {
        close(listener_fd);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(listener_fd, (struct sockaddr *)&address,
                    &address_len) != 0 || address.sin_port == 0) {
        close(listener_fd);
        return ESP_FAIL;
    }

    airplay_event_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) {
        close(listener_fd);
        return ESP_ERR_NO_MEM;
    }
    port->listener_fd = listener_fd;
    port->client_fd = -1;
    port->port = ntohs(address.sin_port);
    memcpy(&port->keys, &config->keys, sizeof(port->keys));
    port->stream_id = config->stream_id;
    port->initial_cseq = config->initial_cseq;
    port->on_state = config->on_state;
    port->context = config->context;
    port->stopped = xSemaphoreCreateBinary();
    if (port->stopped == NULL) {
        close(listener_fd);
        mbedtls_platform_zeroize(port, sizeof(*port));
        free(port);
        return ESP_ERR_NO_MEM;
    }

    BaseType_t task_result = xTaskCreate(event_task, "airplay_event",
                                         EVENT_TASK_STACK_BYTES, port,
                                         EVENT_TASK_PRIORITY, &port->task);
    if (task_result != pdPASS) {
        close(listener_fd);
        vSemaphoreDelete(port->stopped);
        mbedtls_platform_zeroize(port, sizeof(*port));
        free(port);
        return ESP_ERR_NO_MEM;
    }
    *out_port = port;
    ESP_LOGI(TAG, "event_listener_ready ip=" IPSTR " port=%u free_internal=%u free_psram=%u",
             IP2STR(&ap_ip_info.ip), (unsigned)port->port,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

uint16_t airplay_event_port_get_port(const airplay_event_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

void airplay_event_port_stop(airplay_event_port_t *port)
{
    if (port == NULL) return;
    port->stop_requested = true;
    if (port->listener_fd >= 0) shutdown(port->listener_fd, SHUT_RDWR);
    if (port->client_fd >= 0) shutdown(port->client_fd, SHUT_RDWR);
    if (port->task != NULL && xTaskGetCurrentTaskHandle() == port->task) {
        return;
    }
    if (port->task != NULL) {
        (void)xSemaphoreTake(port->stopped, portMAX_DELAY);
    }
    vSemaphoreDelete(port->stopped);
    mbedtls_platform_zeroize(port, sizeof(*port));
    free(port);
}
