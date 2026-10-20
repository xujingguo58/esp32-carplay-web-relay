#include "ed25519.h"

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void from_hex(uint8_t *output, size_t output_len, const char *hex)
{
    for (size_t i = 0U; i < output_len; ++i) {
        unsigned value = 0U;
        assert(sscanf(hex + i * 2U, "%2x", &value) == 1);
        output[i] = (uint8_t)value;
    }
}

int main(void)
{
    static const char seed_hex[] =
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
    static const char public_hex[] =
        "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a";
    static const char signature_hex[] =
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
        "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b";
    uint8_t seed[32];
    uint8_t expected_public[32];
    uint8_t expected_signature[64];
    uint8_t public_key[32];
    uint8_t private_key[64];
    uint8_t signature[64];
    from_hex(seed, sizeof(seed), seed_hex);
    from_hex(expected_public, sizeof(expected_public), public_hex);
    from_hex(expected_signature, sizeof(expected_signature), signature_hex);

    ed25519_create_keypair(public_key, private_key, seed);
    assert(memcmp(public_key, expected_public, sizeof(public_key)) == 0);
    ed25519_sign(signature, NULL, 0U, public_key, private_key);
    assert(memcmp(signature, expected_signature, sizeof(signature)) == 0);
    assert(ed25519_verify(signature, NULL, 0U, public_key) == 1);
    signature[0] ^= 0x01U;
    assert(ed25519_verify(signature, NULL, 0U, public_key) == 0);

    memset(seed, 0, sizeof(seed));
    memset(private_key, 0, sizeof(private_key));
    puts("Ed25519 RFC 8032 vector and tamper rejection passed");
    return 0;
}
