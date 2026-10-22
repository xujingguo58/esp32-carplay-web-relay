#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef void (*bt_port_rx_cb_t)(const uint8_t *data, size_t len, void *context);
typedef void (*bt_port_state_cb_t)(bool connected, void *context);

esp_err_t bt_port_start(bt_port_rx_cb_t rx_cb, bt_port_state_cb_t state_cb, void *context);
/* Discovery is disabled at boot. Enable only after the browser media path is ready.
 * Disabling cancels inquiry/retries; an established RFCOMM link is retained. */
void bt_port_set_discovery_enabled(bool enabled);
esp_err_t bt_port_send(const uint8_t *data, size_t len);
/* Close the active RFCOMM session after receive data loss or protocol desynchronization. */
esp_err_t bt_port_disconnect(void);
bool bt_port_is_connected(void);
uint32_t bt_port_rx_dropped_chunks(void);

