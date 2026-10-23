/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_mfi_sap.h"

airplay_mfi_sap_result_t airplay_mfi_sap_respond(
    const uint8_t *request,
    size_t request_len,
    uint8_t *response,
    size_t response_capacity,
    size_t *response_len)
{
    (void)request;
    (void)request_len;
    (void)response;
    (void)response_capacity;
    if (response_len != NULL) *response_len = 0U;
    return AIRPLAY_MFI_SAP_UNAVAILABLE;
}
