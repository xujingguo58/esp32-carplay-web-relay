#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "web_gateway_pairing.h"

int main(void)
{
    web_gateway_pairing_t pairing;
    assert(web_gateway_pairing_init(&pairing, "a1b2C3d4"));
    assert(web_gateway_pairing_verify(&pairing, "A1B2c3D4", 8, 0));

    assert(!web_gateway_pairing_verify(&pairing, "00000000", 8, 1));
    assert(pairing.failures == 1);
    assert(!web_gateway_pairing_verify(&pairing, "a1b2c3d", 7, 2));
    assert(!web_gateway_pairing_verify(&pairing, "a1b2c3d4x", 9, 3));
    assert(!web_gateway_pairing_verify(&pairing, "a1b2c3g4", 8, 4));
    assert(!web_gateway_pairing_is_locked(&pairing, 4));
    assert(!web_gateway_pairing_verify(&pairing, "00000000", 8, 5));
    assert(web_gateway_pairing_is_locked(&pairing, 5));
    assert(!web_gateway_pairing_verify(&pairing, "a1b2c3d4", 8, 6));
    assert(web_gateway_pairing_is_locked(&pairing, 5 + WEB_GATEWAY_PAIRING_LOCKOUT_MS - 1));
    assert(!web_gateway_pairing_is_locked(&pairing,
                                          5 + WEB_GATEWAY_PAIRING_LOCKOUT_MS));
    assert(web_gateway_pairing_verify(&pairing, "a1b2c3d4", 8,
                                      5 + WEB_GATEWAY_PAIRING_LOCKOUT_MS));
    assert(pairing.failures == 0 && pairing.locked_until_ms == 0);
    assert(!web_gateway_pairing_init(&pairing, "too-short"));
    assert(!web_gateway_pairing_init(&pairing, "1234567g"));

    puts("web_gateway pairing host test passed");
    return 0;
}
