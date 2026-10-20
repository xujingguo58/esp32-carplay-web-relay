/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdint.h>

/* Monotonic time since boot. It is suitable for timeouts and media timestamps,
 * not for wall-clock or certificate-validity decisions. */
int64_t time_port_monotonic_us(void);
int64_t time_port_monotonic_ms(void);
