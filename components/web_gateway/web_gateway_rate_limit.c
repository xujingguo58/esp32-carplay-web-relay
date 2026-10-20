/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "web_gateway_rate_limit.h"

#include <stddef.h>

#define RATE_LIMIT_TOKEN_SCALE 1000U
#define RATE_LIMIT_BURST_MILLI \
    (WEB_GATEWAY_RATE_LIMIT_BURST * RATE_LIMIT_TOKEN_SCALE)

void web_gateway_rate_limiter_reset(web_gateway_rate_limiter_t *limiter,
                                    uint64_t now_ms)
{
    if (limiter == NULL) return;
    limiter->last_refill_ms = now_ms;
    limiter->tokens_milli = RATE_LIMIT_BURST_MILLI;
    limiter->initialized = true;
}

bool web_gateway_rate_limiter_allow(web_gateway_rate_limiter_t *limiter,
                                    uint64_t now_ms)
{
    if (limiter == NULL) return false;
    if (!limiter->initialized) {
        web_gateway_rate_limiter_reset(limiter, now_ms);
    }

    if (now_ms > limiter->last_refill_ms) {
        uint64_t elapsed_ms = now_ms - limiter->last_refill_ms;
        uint64_t refill_milli = elapsed_ms * WEB_GATEWAY_RATE_LIMIT_PER_SECOND;
        uint64_t available_milli = limiter->tokens_milli + refill_milli;
        limiter->tokens_milli = available_milli > RATE_LIMIT_BURST_MILLI
                                    ? RATE_LIMIT_BURST_MILLI
                                    : (uint32_t)available_milli;
        limiter->last_refill_ms = now_ms;
    }

    if (limiter->tokens_milli < RATE_LIMIT_TOKEN_SCALE) return false;
    limiter->tokens_milli -= RATE_LIMIT_TOKEN_SCALE;
    return true;
}
