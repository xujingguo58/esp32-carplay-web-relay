/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "time_port.h"

#include "esp_timer.h"

int64_t time_port_monotonic_us(void)
{
    return esp_timer_get_time();
}

int64_t time_port_monotonic_ms(void)
{
    return time_port_monotonic_us() / 1000;
}
