/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define AIRPLAY_MFI_SAP_MAX_CERTIFICATE_BYTES 1536U
#define AIRPLAY_MFI_SAP_MAX_RESPONSE_BYTES \
    (32U + 4U + AIRPLAY_MFI_SAP_MAX_CERTIFICATE_BYTES + 4U + 64U)

typedef enum {
    AIRPLAY_MFI_SAP_OK = 0,
    AIRPLAY_MFI_SAP_INVALID_ARGUMENT,
    AIRPLAY_MFI_SAP_UNAVAILABLE,
    AIRPLAY_MFI_SAP_UNSUPPORTED_PROTOCOL,
    AIRPLAY_MFI_SAP_CRYPTO_FAILED,
    AIRPLAY_MFI_SAP_SIGN_FAILED,
    AIRPLAY_MFI_SAP_BUFFER_TOO_SMALL,
} airplay_mfi_sap_result_t;

/* Builds the MFiSAP /auth-setup response body from an iPhone request body. */
airplay_mfi_sap_result_t airplay_mfi_sap_respond(
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len);
