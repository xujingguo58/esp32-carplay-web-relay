#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "web_gateway_rate_limit.h"

int main(void)
{
    web_gateway_rate_limiter_t limiter = {0};
    for (unsigned i = 0; i < WEB_GATEWAY_RATE_LIMIT_BURST; ++i) {
        assert(web_gateway_rate_limiter_allow(&limiter, 1000));
    }
    assert(!web_gateway_rate_limiter_allow(&limiter, 1000));
    for (unsigned i = 0; i < WEB_GATEWAY_RATE_LIMIT_PER_SECOND / 2U - 1U; ++i) {
        assert(web_gateway_rate_limiter_allow(&limiter, 1499));
    }
    assert(!web_gateway_rate_limiter_allow(&limiter, 1499));
    assert(web_gateway_rate_limiter_allow(&limiter, 1500));
    for (unsigned i = 0; i < WEB_GATEWAY_RATE_LIMIT_PER_SECOND / 2U; ++i) {
        assert(web_gateway_rate_limiter_allow(&limiter, 2000));
    }
    assert(!web_gateway_rate_limiter_allow(&limiter, 2000));

    web_gateway_rate_limiter_reset(&limiter, 3000);
    assert(web_gateway_rate_limiter_allow(&limiter, 3000));
    assert(web_gateway_rate_limiter_allow(&limiter, 2500));
    puts("web_gateway rate limiter host test passed");
    return 0;
}
