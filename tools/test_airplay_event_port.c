#include <assert.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "airplay_event_port.h"
#include "airplay_control_port.h"
#include "net_port.h"

extern unsigned airplay_event_host_attach_count;
extern unsigned airplay_event_host_detach_count;
extern unsigned airplay_event_host_receive_count;
extern size_t airplay_event_host_last_receive_bytes;

static bool s_last_state;
static unsigned s_state_change_count;
static esp_netif_t s_ap_netif;

esp_netif_t *net_port_get_ap_netif(void) { return &s_ap_netif; }

esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,
                                esp_netif_ip_info_t *ip_info)
{
    if (netif != &s_ap_netif || ip_info == NULL) return ESP_ERR_INVALID_ARG;
    ip_info->ip.addr = htonl(INADDR_LOOPBACK);
    return ESP_OK;
}

static void on_state(bool connected, void *context)
{
    (void)context;
    s_last_state = connected;
    ++s_state_change_count;
}

static void wait_readable(int fd)
{
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(fd, &read_fds);
    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    assert(select(fd + 1, &read_fds, NULL, NULL, &timeout) == 1);
    assert(FD_ISSET(fd, &read_fds));
}

int main(void)
{
    airplay_event_port_config_t config = {0};
    config.on_state = on_state;
    airplay_event_port_t *port = NULL;
    assert(airplay_event_port_start(&config, &port) == ESP_OK);
    assert(port != NULL);
    int listener_fd = airplay_event_port_get_poll_fd(port);
    assert(listener_fd >= 0);

    int client_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    assert(client_fd >= 0);
    struct sockaddr_in address = {0};
    address.sin_family = AF_INET;
    address.sin_port = htons(airplay_event_port_get_port(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(connect(client_fd, (struct sockaddr *)&address, sizeof(address)) == 0);

    wait_readable(listener_fd);
    airplay_event_port_poll(port);
    assert(airplay_event_port_get_poll_fd(port) >= 0);
    assert(airplay_event_host_attach_count == 1U);
    assert(s_state_change_count == 1U && s_last_state);

    static const uint8_t encrypted_message[] = {0x01U, 0x02U, 0x03U, 0x04U};
    assert(send(client_fd, encrypted_message, sizeof(encrypted_message), 0) ==
           (ssize_t)sizeof(encrypted_message));
    int event_fd = airplay_event_port_get_poll_fd(port);
    wait_readable(event_fd);
    airplay_event_port_poll(port);
    assert(airplay_event_host_receive_count == 1U);
    assert(airplay_event_host_last_receive_bytes == sizeof(encrypted_message));

    close(client_fd);
    wait_readable(event_fd);
    airplay_event_port_poll(port);
    assert(airplay_event_host_detach_count == 1U);
    assert(s_state_change_count == 2U && !s_last_state);
    assert(airplay_event_port_get_poll_fd(port) == -1);

    airplay_event_port_stop(port);
    puts("airplay event port host test passed (select polling, attach, receive, cleanup)");
    return 0;
}
