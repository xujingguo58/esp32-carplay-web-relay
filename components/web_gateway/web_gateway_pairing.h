#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_GATEWAY_PAIRING_CODE_LENGTH 8
#define WEB_GATEWAY_PAIRING_MAX_FAILURES 5
#define WEB_GATEWAY_PAIRING_LOCKOUT_MS 30000U

typedef struct {
    char code[WEB_GATEWAY_PAIRING_CODE_LENGTH + 1];
    uint64_t locked_until_ms;
    uint8_t failures;
} web_gateway_pairing_t;

bool web_gateway_pairing_init(web_gateway_pairing_t *pairing, const char *code);
bool web_gateway_pairing_is_locked(const web_gateway_pairing_t *pairing,
                                   uint64_t now_ms);
bool web_gateway_pairing_verify(web_gateway_pairing_t *pairing,
                                const char *candidate,
                                size_t candidate_len,
                                uint64_t now_ms);
