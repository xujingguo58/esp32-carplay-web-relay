#include "web_gateway_pairing.h"

#include <string.h>

static bool valid_code_char(char value)
{
    return (value >= '0' && value <= '9') ||
           (value >= 'a' && value <= 'f') ||
           (value >= 'A' && value <= 'F');
}

static char lower_ascii(char value)
{
    return (value >= 'A' && value <= 'F') ? (char)(value + ('a' - 'A')) : value;
}

bool web_gateway_pairing_init(web_gateway_pairing_t *pairing, const char *code)
{
    if (pairing == NULL || code == NULL ||
        strlen(code) != WEB_GATEWAY_PAIRING_CODE_LENGTH) {
        return false;
    }
    for (size_t i = 0; i < WEB_GATEWAY_PAIRING_CODE_LENGTH; ++i) {
        if (!valid_code_char(code[i])) {
            memset(pairing, 0, sizeof(*pairing));
            return false;
        }
    }
    memset(pairing, 0, sizeof(*pairing));
    memcpy(pairing->code, code, WEB_GATEWAY_PAIRING_CODE_LENGTH);
    for (size_t i = 0; i < WEB_GATEWAY_PAIRING_CODE_LENGTH; ++i) {
        pairing->code[i] = lower_ascii(pairing->code[i]);
    }
    return true;
}

bool web_gateway_pairing_is_locked(const web_gateway_pairing_t *pairing,
                                   uint64_t now_ms)
{
    return pairing != NULL && pairing->locked_until_ms > now_ms;
}

bool web_gateway_pairing_verify(web_gateway_pairing_t *pairing,
                                const char *candidate,
                                size_t candidate_len,
                                uint64_t now_ms)
{
    if (pairing == NULL || candidate == NULL ||
        web_gateway_pairing_is_locked(pairing, now_ms)) {
        return false;
    }

    uint8_t difference = candidate_len == WEB_GATEWAY_PAIRING_CODE_LENGTH ? 0 : 1;
    for (size_t i = 0; i < WEB_GATEWAY_PAIRING_CODE_LENGTH; ++i) {
        char value = i < candidate_len ? candidate[i] : '\0';
        if (!valid_code_char(value)) {
            difference |= 1;
        }
        difference |= (uint8_t)(lower_ascii(value) ^ pairing->code[i]);
    }

    if (difference == 0) {
        pairing->failures = 0;
        pairing->locked_until_ms = 0;
        return true;
    }

    if (pairing->failures < WEB_GATEWAY_PAIRING_MAX_FAILURES) {
        pairing->failures++;
    }
    if (pairing->failures >= WEB_GATEWAY_PAIRING_MAX_FAILURES) {
        pairing->failures = 0;
        pairing->locked_until_ms = UINT64_MAX - now_ms < WEB_GATEWAY_PAIRING_LOCKOUT_MS
                                       ? UINT64_MAX
                                       : now_ms + WEB_GATEWAY_PAIRING_LOCKOUT_MS;
    }
    return false;
}
