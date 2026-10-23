/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_timing_port.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "airplay_timing.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "net_port.h"
#include "time_port.h"

#define TIMING_REQUEST_INTERVAL_US 1000000LL
#define NTP_EPOCH_OFFSET_SECONDS 2208988800ULL

static const char *TAG = "airplay_timing";

struct airplay_timing_port {
    int socket_fd;
    uint16_t port;
    uint16_t peer_port;
    uint32_t peer_ipv4;
    bool request_pending;
    uint64_t pending_timestamp;
    int64_t next_request_us;
};

static uint64_t timing_now_ntp(void)
{
    uint64_t monotonic_us = (uint64_t)time_port_monotonic_us();
    uint64_t seconds = NTP_EPOCH_OFFSET_SECONDS + monotonic_us / 1000000ULL;
    uint64_t fraction = ((monotonic_us % 1000000ULL) << 32U) / 1000000ULL;
    return (seconds << 32U) | fraction;
}

void airplay_timing_port_poll(airplay_timing_port_t *port, bool socket_readable)
{
    if (port == NULL || port->socket_fd < 0) return;

    uint8_t packet[AIRPLAY_TIMING_PACKET_BYTES];
    struct sockaddr_in peer = {
        .sin_family = AF_INET,
        .sin_port = htons(port->peer_port),
        .sin_addr.s_addr = port->peer_ipv4,
    };
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

    if (!socket_readable) return;
    uint8_t incoming[AIRPLAY_TIMING_PACKET_BYTES + 1U];
    struct sockaddr_in source;
    socklen_t source_len = sizeof(source);
    ssize_t received = recvfrom(port->socket_fd, incoming, sizeof(incoming), 0,
                                (struct sockaddr *)&source, &source_len);
    if (received < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
            ESP_LOGW(TAG, "timing_packet=receive_failed errno=%d", errno);
        }
        return;
    }
    if (received != (ssize_t)AIRPLAY_TIMING_PACKET_BYTES ||
        source.sin_addr.s_addr != port->peer_ipv4) return;
    if (incoming[1] == AIRPLAY_TIMING_PT_REQUEST) {
        uint8_t response[AIRPLAY_TIMING_PACKET_BYTES];
        size_t response_len = 0U;
        uint64_t receive_timestamp = timing_now_ntp();
        uint64_t transmit_timestamp = timing_now_ntp();
        if (airplay_timing_build_response(incoming, (size_t)received,
                                          receive_timestamp, transmit_timestamp,
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
    if (socket_fd < 0) {
        ESP_LOGE(TAG, "timing_listener=socket_failed errno=%d free_internal=%u largest_internal=%u free_psram=%u",
                 errno,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_FAIL;
    }
    int reuse = 1;
    (void)setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = ap_ip.ip.addr;
    address.sin_port = 0;
    if (bind(socket_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int socket_errno = errno;
        close(socket_fd);
        ESP_LOGE(TAG, "timing_listener=bind_failed errno=%d", socket_errno);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(socket_fd, (struct sockaddr *)&address, &address_len) != 0 ||
        address.sin_port == 0U) {
        int socket_errno = errno;
        close(socket_fd);
        ESP_LOGE(TAG, "timing_listener=getsockname_failed errno=%d", socket_errno);
        return ESP_FAIL;
    }

    airplay_timing_port_t *port = calloc(1U, sizeof(*port));
    if (port == NULL) {
        close(socket_fd);
        ESP_LOGE(TAG, "timing_listener=state_alloc_failed free_internal=%u largest_internal=%u free_psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    port->socket_fd = socket_fd;
    port->port = ntohs(address.sin_port);
    port->peer_port = config->peer_port;
    port->peer_ipv4 = config->peer_ipv4;
    port->next_request_us = time_port_monotonic_us();
    *out_port = port;
    ESP_LOGI(TAG, "timing_listener=ready ip=" IPSTR " port=%u peer_port=%u mode=rtsp_poll free_internal=%u free_psram=%u",
             IP2STR(&ap_ip.ip), (unsigned)port->port,
             (unsigned)port->peer_port,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

uint16_t airplay_timing_port_get_port(const airplay_timing_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

int airplay_timing_port_get_socket_fd(const airplay_timing_port_t *port)
{
    return port == NULL ? -1 : port->socket_fd;
}

void airplay_timing_port_stop(airplay_timing_port_t *port)
{
    if (port == NULL) return;
    if (port->socket_fd >= 0) close(port->socket_fd);
    memset(port, 0, sizeof(*port));
    free(port);
}
