#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

esp_err_t net_port_start(void);
esp_netif_t *net_port_get_ap_netif(void);
uint8_t net_port_station_count(void);
