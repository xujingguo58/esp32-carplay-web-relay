/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This provider uses a software P-256 identity. It does not emulate or bypass
 * the iAP2 challenge-response exchange; the private key signs each challenge.
 */
#include "auth_software_provider.h"

#include <stdbool.h>
#include <string.h>

#include "auth_port.h"
#include "esp_log.h"
#include "esp_random.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs7.h"

static const char *TAG = "auth_software";

extern const uint8_t identity_pk8_start[] asm("_binary_identity_pk8_start");
extern const uint8_t identity_pk8_end[] asm("_binary_identity_pk8_end");
extern const uint8_t certificate_p7b_start[] asm("_binary_certificate_p7b_start");
extern const uint8_t certificate_p7b_end[] asm("_binary_certificate_p7b_end");

#define MFI_CHALLENGE_SIZE 32U
#define ECDSA_P256_COORDINATE_SIZE 32U
#define ECDSA_P256_RAW_SIGNATURE_SIZE (2U * ECDSA_P256_COORDINATE_SIZE)
#define MFI_CERTIFICATE_MAX_SIZE 1536U

static mbedtls_pk_context s_private_key;
static mbedtls_pkcs7 s_certificate;
static bool s_contexts_initialized;
static bool s_ready;

static int provider_random(void *context, unsigned char *output, size_t output_len)
{
    (void)context;
    if (output == NULL && output_len != 0U) return -1;
    esp_fill_random(output, output_len);
    return 0;
}

static esp_err_t ecdsa_der_to_raw(const unsigned char *der, size_t der_len,
                                  unsigned char output[ECDSA_P256_RAW_SIGNATURE_SIZE])
{
    if (der == NULL || output == NULL || der_len == 0U) return ESP_ERR_INVALID_ARG;

    unsigned char *cursor = (unsigned char *)der;
    const unsigned char *end = der + der_len;
    size_t sequence_len = 0U;
    int ret = mbedtls_asn1_get_tag(&cursor, end, &sequence_len,
                                   MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE);
    if (ret != 0 || sequence_len != (size_t)(end - cursor)) return ESP_ERR_INVALID_RESPONSE;

    const unsigned char *sequence_end = cursor + sequence_len;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    ret = mbedtls_asn1_get_mpi(&cursor, sequence_end, &r);
    if (ret == 0) ret = mbedtls_asn1_get_mpi(&cursor, sequence_end, &s);
    if (ret == 0 && cursor != sequence_end) ret = MBEDTLS_ERR_ASN1_LENGTH_MISMATCH;
    if (ret == 0 && (mbedtls_mpi_cmp_int(&r, 0) <= 0 ||
                     mbedtls_mpi_cmp_int(&s, 0) <= 0 ||
                     mbedtls_mpi_bitlen(&r) > 256U ||
                     mbedtls_mpi_bitlen(&s) > 256U)) {
        ret = MBEDTLS_ERR_ASN1_INVALID_DATA;
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&r, output, ECDSA_P256_COORDINATE_SIZE);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_write_binary(&s, output + ECDSA_P256_COORDINATE_SIZE,
                                       ECDSA_P256_COORDINATE_SIZE);
    }
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);
    return ret == 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;
}

static esp_err_t read_certificate(uint8_t *out, size_t *inout_len, void *context)
{
    (void)context;
    if (out == NULL || inout_len == NULL || !s_ready) return ESP_ERR_INVALID_STATE;
    const size_t certificate_len = (size_t)(certificate_p7b_end - certificate_p7b_start);
    if (*inout_len < certificate_len) {
        *inout_len = certificate_len;
        return ESP_ERR_NO_MEM;
    }
    memcpy(out, certificate_p7b_start, certificate_len);
    *inout_len = certificate_len;
    return ESP_OK;
}

static esp_err_t sign_challenge(const uint8_t *challenge, size_t challenge_len,
                                uint8_t *signature, size_t *inout_signature_len,
                                void *context)
{
    (void)context;
    if (challenge == NULL || signature == NULL || inout_signature_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) return ESP_ERR_INVALID_STATE;
    if (challenge_len != MFI_CHALLENGE_SIZE) return ESP_ERR_INVALID_SIZE;
    if (*inout_signature_len < ECDSA_P256_RAW_SIGNATURE_SIZE) {
        *inout_signature_len = ECDSA_P256_RAW_SIGNATURE_SIZE;
        return ESP_ERR_NO_MEM;
    }

    unsigned char der_signature[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_signature_len = 0U;
    int ret = mbedtls_pk_sign(&s_private_key, MBEDTLS_MD_SHA256,
                              challenge, challenge_len,
                              der_signature, sizeof(der_signature), &der_signature_len,
                              provider_random, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "challenge signature failed: mbedtls=%d", ret);
        memset(der_signature, 0, sizeof(der_signature));
        *inout_signature_len = 0U;
        return ESP_FAIL;
    }

    esp_err_t err = ecdsa_der_to_raw(der_signature, der_signature_len, signature);
    memset(der_signature, 0, sizeof(der_signature));
    if (err != ESP_OK) {
        memset(signature, 0, ECDSA_P256_RAW_SIGNATURE_SIZE);
        *inout_signature_len = 0U;
        return err;
    }
    *inout_signature_len = ECDSA_P256_RAW_SIGNATURE_SIZE;
    return ESP_OK;
}

static void clear_contexts(void)
{
    if (s_contexts_initialized) {
        mbedtls_pk_free(&s_private_key);
        mbedtls_pkcs7_free(&s_certificate);
    }
    mbedtls_pk_init(&s_private_key);
    mbedtls_pkcs7_init(&s_certificate);
    s_contexts_initialized = true;
    s_ready = false;
}

esp_err_t auth_software_provider_init(void)
{
    if (s_ready) return ESP_OK;
    if (!s_contexts_initialized) {
        mbedtls_pk_init(&s_private_key);
        mbedtls_pkcs7_init(&s_certificate);
        s_contexts_initialized = true;
    } else {
        clear_contexts();
    }

    const size_t private_key_len = (size_t)(identity_pk8_end - identity_pk8_start);
    const size_t certificate_len = (size_t)(certificate_p7b_end - certificate_p7b_start);
    if (private_key_len == 0U || certificate_len == 0U ||
        certificate_len > MFI_CERTIFICATE_MAX_SIZE) {
        ESP_LOGE(TAG, "bundled identity has invalid size");
        clear_contexts();
        return ESP_ERR_INVALID_SIZE;
    }

    int ret = mbedtls_pk_parse_key(&s_private_key, identity_pk8_start, private_key_len,
                                   NULL, 0U, provider_random, NULL);
    if (ret != 0 || !mbedtls_pk_can_do(&s_private_key, MBEDTLS_PK_ECDSA) ||
        mbedtls_pk_get_bitlen(&s_private_key) != 256U) {
        ESP_LOGE(TAG, "bundled P-256 private key parse failed: mbedtls=%d", ret);
        clear_contexts();
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_pkcs7_parse_der(&s_certificate, certificate_p7b_start, certificate_len);
    if (ret != MBEDTLS_PKCS7_SIGNED_DATA ||
        s_certificate.MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(no_of_certs) != 1) {
        ESP_LOGE(TAG, "bundled certificate parse failed: mbedtls=%d", ret);
        clear_contexts();
        return ESP_ERR_INVALID_ARG;
    }

    mbedtls_x509_crt *certificate =
        &s_certificate.MBEDTLS_PRIVATE(signed_data).MBEDTLS_PRIVATE(certs);
    mbedtls_pk_context *certificate_key = &certificate->pk;
    if (!mbedtls_pk_can_do(certificate_key, MBEDTLS_PK_ECDSA) ||
        mbedtls_pk_get_bitlen(certificate_key) != 256U) {
        ESP_LOGE(TAG, "bundled certificate does not contain a P-256 ECDSA key");
        clear_contexts();
        return ESP_ERR_INVALID_ARG;
    }

    ret = mbedtls_pk_check_pair(certificate_key, &s_private_key, provider_random, NULL);
    if (ret != 0) {
        ESP_LOGE(TAG, "bundled private key does not match certificate: mbedtls=%d", ret);
        clear_contexts();
        return ESP_ERR_INVALID_ARG;
    }

    const auth_port_provider_t provider = {
        .read_certificate = read_certificate,
        .sign_challenge = sign_challenge,
        .context = NULL,
    };
    esp_err_t err = auth_port_register_provider(&provider);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "auth_port registration failed: %s", esp_err_to_name(err));
        clear_contexts();
        return err;
    }

    s_ready = true;
    ESP_LOGW(TAG, "software MFi challenge signer ready; bundled private key is extractable from firmware");
    return ESP_OK;
}
