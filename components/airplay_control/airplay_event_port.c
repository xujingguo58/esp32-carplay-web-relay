/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_event_port.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#include "airplay_control_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "mbedtls/platform_util.h"
#include "net_port.h"

#define EVENT_RX_CHUNK_BYTES 1200U

static const char *TAG = "airplay_event";

struct airplay_event_port {
    int listener_fd;
    int client_fd;
    uint16_t port;
    bool attached;
    bool connected_notified;
    bool failed;
    bool state_in_psram;
    airplay_pair_verify_event_keys_t keys;
    uint32_t stream_id;
    uint32_t initial_cseq;
    airplay_event_port_state_cb_t on_state;
    void *context;
    uint8_t rx[EVENT_RX_CHUNK_BYTES];
};

static void log_alloc_failure(const char *stage)
{
    ESP_LOGE(TAG,
             "event_alloc=failed stage=%s free_internal=%u largest_internal=%u free_psram=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void *event_calloc(size_t size, bool *allocated_in_psram)
{
    if (allocated_in_psram != NULL) *allocated_in_psram = false;
    void *memory = heap_caps_calloc(1U, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (memory != NULL) {
        if (allocated_in_psram != NULL) *allocated_in_psram = true;
        return memory;
    }
    return heap_caps_calloc(1U, size,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void close_socket(int *fd)
{
    if (fd == NULL || *fd < 0) return;
    shutdown(*fd, SHUT_RDWR);
    close(*fd);
    *fd = -1;
}

static bool set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void notify_state(airplay_event_port_t *port, bool connected)
{
    if (port == NULL) return;
    port->connected_notified = connected;
    if (port->on_state != NULL) port->on_state(connected, port->context);
}

static void fail_event_port(airplay_event_port_t *port, const char *reason)
{
    if (port == NULL || port->failed) return;
    ESP_LOGW(TAG, "event_connection=failed reason=%s", reason);
    if (port->attached) {
        airplay_control_port_detach();
        port->attached = false;
    }
    mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
    close_socket(&port->client_fd);
    close_socket(&port->listener_fd);
    port->failed = true;
    /* A failed listener/connection is reported so the session can prevent
     * unsafe event-key reuse after its sequence counters have been consumed. */
    notify_state(port, false);
}

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

esp_err_t airplay_event_port_start(const airplay_event_port_config_t *config,
                                   airplay_event_port_t **out_port)
{
    if (out_port == NULL) return ESP_ERR_INVALID_ARG;
    *out_port = NULL;
    if (config == NULL) return ESP_ERR_INVALID_ARG;
    esp_err_t control_result = airplay_control_port_init();
    if (control_result != ESP_OK) {
        log_alloc_failure("control_mutex");
        return control_result;
    }

    int listener_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener_fd < 0) {
        ESP_LOGE(TAG, "event_listener=socket_failed errno=%d free_internal=%u largest_internal=%u",
                 errno,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return ESP_FAIL;
    }
    if (!set_nonblocking(listener_fd)) {
        int socket_errno = errno;
        close(listener_fd);
        ESP_LOGE(TAG, "event_listener=nonblocking_failed errno=%d", socket_errno);
        return ESP_FAIL;
    }
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
    if (bind(listener_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
        int socket_errno = errno;
        close(listener_fd);
        ESP_LOGE(TAG, "event_listener=bind_failed errno=%d address=" IPSTR,
                 socket_errno, IP2STR(&ap_ip_info.ip));
        return ESP_FAIL;
    }
    if (listen(listener_fd, 1) != 0) {
        int socket_errno = errno;
        close(listener_fd);
        ESP_LOGE(TAG, "event_listener=listen_failed errno=%d", socket_errno);
        return ESP_FAIL;
    }
    socklen_t address_len = sizeof(address);
    if (getsockname(listener_fd, (struct sockaddr *)&address,
                    &address_len) != 0 || address.sin_port == 0U) {
        int socket_errno = errno;
        close(listener_fd);
        ESP_LOGE(TAG, "event_listener=getsockname_failed errno=%d port=%u",
                 socket_errno, (unsigned)ntohs(address.sin_port));
        return ESP_FAIL;
    }

    bool state_in_psram = false;
    airplay_event_port_t *port = event_calloc(sizeof(*port), &state_in_psram);
    if (port == NULL) {
        close(listener_fd);
        log_alloc_failure("listener_state");
        return ESP_ERR_NO_MEM;
    }
    port->listener_fd = listener_fd;
    port->client_fd = -1;
    port->port = ntohs(address.sin_port);
    port->state_in_psram = state_in_psram;
    memcpy(&port->keys, &config->keys, sizeof(port->keys));
    port->stream_id = config->stream_id;
    port->initial_cseq = config->initial_cseq;
    port->on_state = config->on_state;
    port->context = config->context;
    *out_port = port;
    ESP_LOGI(TAG, "event_listener=ready ip=" IPSTR " port=%u state_memory=%s state_bytes=%u mode=rtsp_poll free_internal=%u free_psram=%u",
             IP2STR(&ap_ip_info.ip), (unsigned)port->port,
             port->state_in_psram ? "psram" : "internal",
             (unsigned)sizeof(*port),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

uint16_t airplay_event_port_get_port(const airplay_event_port_t *port)
{
    return port == NULL ? 0U : port->port;
}

int airplay_event_port_get_poll_fd(const airplay_event_port_t *port)
{
    if (port == NULL || port->failed) return -1;
    if (port->client_fd >= 0) return port->client_fd;
    return port->listener_fd;
}

void airplay_event_port_poll(airplay_event_port_t *port)
{
    if (port == NULL || port->failed) return;

    if (port->client_fd < 0) {
        struct sockaddr_in peer_address;
        socklen_t peer_address_len = sizeof(peer_address);
        int client_fd = accept(port->listener_fd,
                               (struct sockaddr *)&peer_address,
                               &peer_address_len);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) return;
            fail_event_port(port, "accept");
            return;
        }
        int no_delay = 1;
        (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                         &no_delay, sizeof(no_delay));
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 500000};
        (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO,
                         &timeout, sizeof(timeout));
        (void)setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO,
                         &timeout, sizeof(timeout));
        port->client_fd = client_fd;
        close_socket(&port->listener_fd);
        port->attached = airplay_control_port_attach(
            port->keys.read_key, port->keys.write_key,
            port->stream_id, port->initial_cseq,
            socket_write, port) == ESP_OK;
        mbedtls_platform_zeroize(&port->keys, sizeof(port->keys));
        if (!port->attached) {
            fail_event_port(port, "control_attach");
            return;
        }
        ESP_LOGI(TAG, "event_connected peer=%s:%u stream_id=%" PRIu32,
                 inet_ntoa(peer_address.sin_addr),
                 (unsigned)ntohs(peer_address.sin_port), port->stream_id);
        notify_state(port, true);
        return;
    }

    int received = recv(port->client_fd, port->rx, sizeof(port->rx), MSG_DONTWAIT);
    if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) return;
    if (received == 0) {
        fail_event_port(port, "peer_closed");
        return;
    }
    if (received < 0) {
        fail_event_port(port, "receive");
        return;
    }
    esp_err_t receive_result = airplay_control_port_receive_encrypted(
        port->rx, (size_t)received);
    if (receive_result != ESP_OK) {
        ESP_LOGW(TAG, "event_rx_rejected error=%s", esp_err_to_name(receive_result));
        fail_event_port(port, "encrypted_payload");
        return;
    }
    mbedtls_platform_zeroize(port->rx, (size_t)received);
}

void airplay_event_port_stop(airplay_event_port_t *port)
{
    if (port == NULL) return;
    if (port->attached) {
        airplay_control_port_detach();
        port->attached = false;
    }
    if (port->connected_notified) notify_state(port, false);
    close_socket(&port->client_fd);
    close_socket(&port->listener_fd);
    mbedtls_platform_zeroize(port, sizeof(*port));
    heap_caps_free(port);
}
