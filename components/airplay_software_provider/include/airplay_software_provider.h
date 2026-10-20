/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"

/* Loads or creates a persistent AirPlay accessory identity and registers the
 * Pair-Setup/Pair-Verify, /info, and Bonjour configuration provider. */
esp_err_t airplay_software_provider_init(void);
