/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "esp_err.h"

/* Load the bundled experimental P-256 identity, validate its key pair, and
 * register it as the iAP2 MFi challenge-response provider. */
esp_err_t auth_software_provider_init(void);
