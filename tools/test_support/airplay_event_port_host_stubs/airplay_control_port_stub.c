#include "airplay_control_port.h"

unsigned airplay_event_host_attach_count;
unsigned airplay_event_host_detach_count;
unsigned airplay_event_host_receive_count;
size_t airplay_event_host_last_receive_bytes;

esp_err_t airplay_control_port_init(void) { return ESP_OK; }

esp_err_t airplay_control_port_attach(
    const uint8_t read_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    const uint8_t write_key[AIRPLAY_PAIR_VERIFY_KEY_BYTES],
    uint32_t stream_id, uint32_t initial_cseq,
    airplay_control_write_cb_t write_cb, void *context)
{
    (void)read_key;
    (void)write_key;
    (void)stream_id;
    (void)initial_cseq;
    (void)write_cb;
    (void)context;
    ++airplay_event_host_attach_count;
    return ESP_OK;
}

void airplay_control_port_detach(void) { ++airplay_event_host_detach_count; }

esp_err_t airplay_control_port_receive_encrypted(const uint8_t *bytes,
                                                  size_t len)
{
    if (bytes == NULL || len == 0U) return ESP_ERR_INVALID_ARG;
    ++airplay_event_host_receive_count;
    airplay_event_host_last_receive_bytes = len;
    return ESP_OK;
}
