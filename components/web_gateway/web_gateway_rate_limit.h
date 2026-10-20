/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define WEB_GATEWAY_RATE_LIMIT_BURST 120U
#define WEB_GATEWAY_RATE_LIMIT_PER_SECOND 120U

typedef struct {
    uint64_t last_refill_ms;
    uint32_t tokens_milli;
    bool initialized;
} web_gateway_rate_limiter_t;

void web_gateway_rate_limiter_reset(web_gateway_rate_limiter_t *limiter,
                                    uint64_t now_ms);
bool web_gateway_rate_limiter_allow(web_gateway_rate_limiter_t *limiter,
                                    uint64_t now_ms);
