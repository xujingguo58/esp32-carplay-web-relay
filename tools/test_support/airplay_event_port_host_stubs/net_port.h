#pragma once

#include <stdint.h>

#include "esp_err.h"

typedef struct { uint32_t addr; } esp_ip4_addr_t;
typedef struct { esp_ip4_addr_t ip; } esp_netif_ip_info_t;
typedef struct { int unused; } esp_netif_t;

esp_netif_t *net_port_get_ap_netif(void);
esp_err_t esp_netif_get_ip_info(esp_netif_t *netif,
                                esp_netif_ip_info_t *ip_info);
