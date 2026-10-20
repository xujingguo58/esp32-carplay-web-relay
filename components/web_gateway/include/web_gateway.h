#pragma once

#include <stdint.h>

#include "esp_err.h"

/* Must be set before web_gateway_start; no provider keeps M2 sample geometry. */
esp_err_t web_gateway_set_display_geometry(uint16_t width_pixels,
                                           uint16_t height_pixels,
                                           uint16_t rotation_degrees);
/* Queue touch cleanup onto the HTTP server task from transport callbacks. */
esp_err_t web_gateway_release_touches(void);
esp_err_t web_gateway_start(void);
