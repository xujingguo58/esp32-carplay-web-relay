/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing_port.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "airplay_timing.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net_port.h"
#include "time_port.h"

#define TIMING_TASK_STACK_BYTES 3072U
#define TIMING_TASK_PRIORITY 4U
#define TIMING_REQUEST_INTERVAL_US 1000000LL
#define TIMING_POLL_TIMEOUT_MS 250U
#define NTP_EPOCH_OFFSET_SECONDS 2208988800ULL

static const char *TAG = "airplay_timing";

struct airplay_timing_port {
    int socket_fd;
    uint16_t port;
    uint16_t peer_port;
    uint32_t peer_ipv4;
    volatile bool stop_requested;
    bool request_pending;
    uint64_t pending_timestamp;
    int64_t next_request_us;
    TaskHandle_t task;
    SemaphoreHandle_t stopped;
};

static uint64_t timing_now_ntp(void)
{
    uint64_t monotonic_us = (uint64_t)time_port_monotonic_us();
    uint64_t seconds = NTP_EPOCH_OFFSET_SECONDS + monotonic_us / 1000000ULL;
    uint64_t fraction = ((monotonic_us % 1000000ULL) << 32U) / 1000000ULL;
    return (seconds << 32U) | fraction;
}

static void timing_task(void *argument)
{
    airplay_timing_port_t *port = argument;
    uint8_t packet[AIRPLAY_TIMING_PACKET_BYTES];
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
        .sin_port = htons(port->peer_port),
        .sin_addr.s_addr = port->peer_ipv4,
    };
    port->next_request_us = time_port_monotonic_us();
    while (!port->stop_requested) {
        int64_t now_us = time_port_monotonic_us();
        if (port->peer_port != 0U && now_us >= port->next_request_us) {
            size_t request_len = 0U;
            uint64_t timestamp = timing_now_ntp();
            port->request_pending = false;
            if (airplay_timing_build_request(timestamp, packet, sizeof(packet),
                                             &request_len) &&
                sendto(port->socket_fd, packet, request_len, 0,
                       (struct sockaddr *)&peer, sizeof(peer)) ==
                    (ssize_t)request_len) {
                port->pending_timestamp = timestamp;
                port->request_pending = true;
            }
            port->next_request_us = now_us + TIMING_REQUEST_INTERVAL_US;
        }

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(port->socket_fd, &read_fds);
        struct timeval timeout = {
            .tv_sec = TIMING_POLL_TIMEOUT_MS / 1000U,
            .tv_usec = (TIMING_POLL_TIMEOUT_MS % 1000U) * 1000U,
        };
        int selected = select(port->socket_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (selected < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (selected == 0 || !FD_ISSET(port->socket_fd, &read_fds)) continue;

        uint8_t incoming[AIRPLAY_TIMING_PACKET_BYTES + 1U];
        struct sockaddr_in source;
        socklen_t source_len = sizeof(source);
        ssize_t received = recvfrom(port->socket_fd, incoming, sizeof(incoming), 0,
                                    (struct sockaddr *)&source, &source_len);
        if (received != (ssize_t)AIRPLAY_TIMING_PACKET_BYTES ||
            source.sin_addr.s_addr != port->peer_ipv4) continue;
        if (incoming[1] == AIRPLAY_TIMING_PT_REQUEST) {
            uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
            size_t response_len = 0U;
            if (airplay_timing_build_response(incoming, (size_t)received,
                                              timing_now_ntp(), timing_now_ntp(),
                                              response, sizeof(response),
                                              &response_len)) {
                (void)sendto(port->socket_fd, response, response_len, 0,
                             (struct sockaddr *)&source, source_len);
            }
        } else if (incoming[1] == AIRPLAY_TIMING_PT_RESPONSE &&
                   port->request_pending) {
            double rtt_seconds = 0.0;
            double offset_seconds = 0.0;
            if (airplay_timing_parse_response(incoming, (size_t)received,
                                              port->pending_timestamp,
                                              timing_now_ntp(), &rtt_seconds,
                                              &offset_seconds)) {
                ESP_LOGD(TAG, "clock_sample rtt_ms=%.3f offset_ms=%.3f",
                         rtt_seconds * 1000.0, offset_seconds * 1000.0);
                port->request_pending = false;
            }
        }
    }
    memset(packet, 0, sizeof(packet));
    close(port->socket_fd);
    port->socket_fd = -1;
    ESP_LOGI(TAG, "timing_listener=stopped stack_hwm=%u",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    (void)xSemaphoreGive(port->stopped);
    vTaskDelete(NULL);
}

esp_err_t airplay_timing_port_start(const airplay_timing_port_config_t *config,
                                    airplay_timing_port_t **out_port)
{
    if (out_port == NULL) return ESP_ERR_INVALID_ARG;
    *out_port = NULL;
    if (config == NULL || config->peer_ipv4 == 0U) return ESP_ERR_INVALID_ARG;

    esp_netif_t *ap_netif = net_port_get_ap_netif();
    esp_netif_ip_info_t ap_ip;
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ap_ip) != ESP_OK ||
        ap_ip.ip.addr == 0U) return ESP_ERR_INVALID_STATE;

    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_fd < 0) return ESP_FAIL;
    int reuse = 1;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ap_ip.ip.addr;
    address.sin_port = 0;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(socket_fd);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr *)&address, &address_len) != 0 ||
        address.sin_port == 0U) {
        close(socket_fd);
        return ESP_FAIL;
    }

    airplay_timing_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) {
        close(socket_fd);
        return ESP_ERR_NO_MEM;
    }
    port->socket_fd = socket_fd;
    port->port = ntohs(address.sin_port);
    port->peer_port = config->peer_port;
    port->peer_ipv4 = config->peer_ipv4;
    port->stopped = xSemaphoreCreateBinary();
    if (port->stopped == NULL) {
        close(socket_fd);
        free(port);
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(timing_task, "airplay_timing", TIMING_TASK_STACK_BYTES,
                    port, TIMING_TASK_PRIORITY, &port->task) != pdPASS) {
        close(socket_fd);
        vSemaphoreDelete(port->stopped);
        free(port);
        return ESP_ERR_NO_MEM;
    }
    *out_port = port;
    ESP_LOGI(TAG, "timing_listener=ready ip=" IPSTR " port=%u peer_port=%u free_internal=%u",
             IP2STR(&ap_ip.ip), (unsigned)port->port,
             (unsigned)port->peer_port,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_OK;
}

uint16_t airplay_timing_port_get_port(const airplay_timing_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

void airplay_timing_port_stop(airplay_timing_port_t *port)
{
    if (port == NULL) return;
    port->stop_requested = true;
    if (port->task != NULL && xTaskGetCurrentTaskHandle() == port->task) return;
    (void)xSemaphoreTake(port->stopped, portMAX_DELAY);
    vSemaphoreDelete(port->stopped);
    memset(port, 0, sizeof(*port));
    free(port);
}
