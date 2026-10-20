/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * DiPlay-compatible software AirPlay Pair-Setup/Pair-Verify provider. The
 * receiver identity is generated once and retained in NVS; it is separate
 * from the bundled iAP2 MFi P-256 identity.
 */
#include "airplay_software_provider.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "airplay_hid_descriptor.h"
#include "airplay_identity_port.h"
#include "airplay_pair_setup.h"
#include "airplay_pairing_store.h"
#include "ed25519.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "airplay_identity";

extern const uint8_t info_plist_start[] asm("_binary_info_plist_start");
extern const uint8_t info_plist_end[] asm("_binary_info_plist_end");

#define IDENTITY_NAMESPACE "airplay_id"
#define IDENTITY_KEY "receiver"
#define IDENTITY_STORE_BYTES 74U
#define IDENTITY_SEED_OFFSET 5U
#define IDENTITY_PAIRING_ID_OFFSET 37U
#define IDENTITY_PAIRING_ID_BYTES 36U
#define AIRPLAY_DISPLAY_WIDTH 800U
#define AIRPLAY_DISPLAY_HEIGHT 480U
#define AIRPLAY_INFO_MAX_BYTES 4096U
#define AIRPLAY_PUBLIC_KEY_HEX_BYTES 64U
#define AIRPLAY_DEVICE_ID_BYTES 17U
#define AIRPLAY_SOURCE_VERSION "950.7.1"
#define AIRPLAY_FEATURES_TXT "0x5203a4e2,0x61"
#define AIRPLAY_MAIN_DISPLAY_UUID "b7e6c5a0-1111-4000-8000-000000000001"

static const uint8_t k_store_magic[4] = {'A', 'P', 'I', '1'};
static const uint8_t k_info_device_id_placeholder[AIRPLAY_DEVICE_ID_BYTES] =
    "00:00:00:00:00:01";
static const uint8_t k_info_bt_id_placeholder[AIRPLAY_DEVICE_ID_BYTES] =
    "00:00:00:00:00:02";

static uint8_t s_public_key[32];
static uint8_t s_private_key[64];
static char s_pairing_id[IDENTITY_PAIRING_ID_BYTES + 1U];
static char s_device_id[AIRPLAY_DEVICE_ID_BYTES + 1U];
static char s_bluetooth_id[AIRPLAY_DEVICE_ID_BYTES + 1U];
static char s_public_key_hex[AIRPLAY_PUBLIC_KEY_HEX_BYTES + 1U];
static uint8_t s_info_plist[AIRPLAY_INFO_MAX_BYTES];
static size_t s_info_plist_len;
static bool s_ready;

static const char *const s_mdns_hostname = "esp32-carplay";
static const char *const s_mdns_instance = "ESP32 CarPlay Relay";
static const char *const s_model = "ESP32";
static const char *const s_protocol_version = "1.1";
static const char *const s_pairing_code = CONFIG_AIRPLAY_PAIRING_CODE;

static airplay_mdns_txt_record_t s_mdns_txt[] = {
    {.key = "deviceid", .value = s_device_id},
    {.key = "features", .value = AIRPLAY_FEATURES_TXT},
    {.key = "flags", .value = "0x4"},
    {.key = "model", .value = s_model},
    {.key = "srcvers", .value = AIRPLAY_SOURCE_VERSION},
    {.key = "protovers", .value = s_protocol_version},
    {.key = "pi", .value = s_pairing_id},
    {.key = "pk", .value = s_public_key_hex},
};

static bool identifier_valid(const char *identifier)
{
    if (identifier == NULL || strlen(identifier) != IDENTITY_PAIRING_ID_BYTES) {
        return false;
    }
    for (size_t i = 0U; i < IDENTITY_PAIRING_ID_BYTES; ++i) {
        if (i == 8U || i == 13U || i == 18U || i == 23U) {
            if (identifier[i] != '-') return false;
        } else if (!((identifier[i] >= '0' && identifier[i] <= '9') ||
                     (identifier[i] >= 'a' && identifier[i] <= 'f'))) {
            return false;
        }
    }
    return true;
}

static void format_uuid(char output[IDENTITY_PAIRING_ID_BYTES + 1U],
                        uint8_t uuid[16])
{
    uuid[6] = (uint8_t)((uuid[6] & 0x0fU) | 0x40U);
    uuid[8] = (uint8_t)((uuid[8] & 0x3fU) | 0x80U);
    (void)snprintf(output, IDENTITY_PAIRING_ID_BYTES + 1U,
                   "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
                   "%02x%02x%02x%02x%02x%02x",
                   uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5],
                   uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11],
                   uuid[12], uuid[13], uuid[14], uuid[15]);
}

static esp_err_t load_or_create_identity(void)
{
    uint8_t record[IDENTITY_STORE_BYTES] = {0};
    nvs_handle_t handle;
    esp_err_t err = nvs_open(IDENTITY_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    size_t record_len = sizeof(record);
    err = nvs_get_blob(handle, IDENTITY_KEY, record, &record_len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t uuid[16];
        memcpy(record, k_store_magic, sizeof(k_store_magic));
        record[4] = 1U;
        esp_fill_random(record + IDENTITY_SEED_OFFSET, 32U);
        esp_fill_random(uuid, sizeof(uuid));
        char pairing_id[IDENTITY_PAIRING_ID_BYTES + 1U];
        format_uuid(pairing_id, uuid);
        memcpy(record + IDENTITY_PAIRING_ID_OFFSET, pairing_id,
               IDENTITY_PAIRING_ID_BYTES);
        record[IDENTITY_PAIRING_ID_OFFSET + IDENTITY_PAIRING_ID_BYTES] = 0U;
        err = nvs_set_blob(handle, IDENTITY_KEY, record, sizeof(record));
        if (err == ESP_OK) err = nvs_commit(handle);
        memset(uuid, 0, sizeof(uuid));
        memset(pairing_id, 0, sizeof(pairing_id));
        if (err != ESP_OK) {
            memset(record, 0, sizeof(record));
            nvs_close(handle);
            return err;
        }
        record_len = sizeof(record);
    } else if (err != ESP_OK) {
        memset(record, 0, sizeof(record));
        nvs_close(handle);
        return err;
    }
    nvs_close(handle);

    if (record_len != sizeof(record) ||
        memcmp(record, k_store_magic, sizeof(k_store_magic)) != 0 ||
        record[4] != 1U ||
        record[IDENTITY_PAIRING_ID_OFFSET + IDENTITY_PAIRING_ID_BYTES] != 0U) {
        memset(record, 0, sizeof(record));
        return ESP_ERR_INVALID_CRC;
    }
    memcpy(s_pairing_id, record + IDENTITY_PAIRING_ID_OFFSET,
           IDENTITY_PAIRING_ID_BYTES);
    s_pairing_id[IDENTITY_PAIRING_ID_BYTES] = '\0';
    if (!identifier_valid(s_pairing_id)) {
        memset(record, 0, sizeof(record));
        memset(s_pairing_id, 0, sizeof(s_pairing_id));
        return ESP_ERR_INVALID_CRC;
    }

    ed25519_create_keypair(s_public_key, s_private_key,
                           record + IDENTITY_SEED_OFFSET);
    memset(record, 0, sizeof(record));
    return ESP_OK;
}

static esp_err_t format_device_id(void)
{
    uint8_t digest[32];
    int result = mbedtls_sha256(s_public_key, sizeof(s_public_key), digest, 0);
    if (result != 0) {
        memset(digest, 0, sizeof(digest));
        return ESP_FAIL;
    }
    /* DiPlay derives a locally administered, unicast device ID from SHA-256(public key). */
    digest[0] = (uint8_t)((digest[0] & 0xfcU) | 0x02U);
    (void)snprintf(s_device_id, sizeof(s_device_id), "%02X:%02X:%02X:%02X:%02X:%02X",
                   digest[0], digest[1], digest[2], digest[3], digest[4], digest[5]);
    memset(digest, 0, sizeof(digest));

    uint8_t bluetooth_mac[6];
    esp_err_t err = esp_read_mac(bluetooth_mac, ESP_MAC_BT);
    if (err != ESP_OK) return err;
    (void)snprintf(s_bluetooth_id, sizeof(s_bluetooth_id),
                   "%02X:%02X:%02X:%02X:%02X:%02X",
                   bluetooth_mac[0], bluetooth_mac[1], bluetooth_mac[2],
                   bluetooth_mac[3], bluetooth_mac[4], bluetooth_mac[5]);
    memset(bluetooth_mac, 0, sizeof(bluetooth_mac));

    static const char k_hex[] = "0123456789abcdef";
    for (size_t i = 0U; i < sizeof(s_public_key); ++i) {
        s_public_key_hex[i * 2U] = k_hex[s_public_key[i] >> 4U];
        s_public_key_hex[i * 2U + 1U] = k_hex[s_public_key[i] & 0x0fU];
    }
    s_public_key_hex[AIRPLAY_PUBLIC_KEY_HEX_BYTES] = '\0';
    return ESP_OK;
}

static bool replace_once(uint8_t *buffer, size_t buffer_len,
                         const uint8_t *needle, size_t needle_len,
                         const uint8_t *replacement)
{
    if (buffer == NULL || needle == NULL || replacement == NULL ||
        needle_len == 0U || needle_len > buffer_len) {
        return false;
    }
    size_t match = SIZE_MAX;
    for (size_t i = 0U; i <= buffer_len - needle_len; ++i) {
        if (memcmp(buffer + i, needle, needle_len) == 0) {
            if (match != SIZE_MAX) return false;
            match = i;
        }
    }
    if (match == SIZE_MAX) return false;
    memcpy(buffer + match, replacement, needle_len);
    return true;
}

static esp_err_t prepare_info_plist(void)
{
    const size_t template_len = (size_t)(info_plist_end - info_plist_start);
    if (template_len == 0U || template_len > sizeof(s_info_plist)) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(s_info_plist, info_plist_start, template_len);
    if (!replace_once(s_info_plist, template_len,
                      k_info_device_id_placeholder,
                      sizeof(k_info_device_id_placeholder),
                      (const uint8_t *)s_device_id) ||
        !replace_once(s_info_plist, template_len,
                      k_info_bt_id_placeholder,
                      sizeof(k_info_bt_id_placeholder),
                      (const uint8_t *)s_bluetooth_id)) {
        memset(s_info_plist, 0, sizeof(s_info_plist));
        return ESP_ERR_INVALID_RESPONSE;
    }
    s_info_plist_len = template_len;
    return ESP_OK;
}

static bool provider_random(uint8_t *output, size_t output_len, void *context)
{
    (void)context;
    if (output == NULL && output_len != 0U) return false;
    esp_fill_random(output, output_len);
    return true;
}

static bool sign_accessory(const uint8_t *message, size_t message_len,
                           uint8_t signature[AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES],
                           void *context)
{
    (void)context;
    if (!s_ready || signature == NULL || (message == NULL && message_len != 0U)) {
        return false;
    }
    static const uint8_t k_empty_message = 0U;
    if (message == NULL) message = &k_empty_message;
    ed25519_sign(signature, message, message_len, s_public_key, s_private_key);
    return true;
}

static bool verify_controller(
    const uint8_t public_key[AIRPLAY_PAIR_SETUP_KEY_BYTES],
    const uint8_t *message, size_t message_len,
    const uint8_t signature[AIRPLAY_PAIR_SETUP_SIGNATURE_BYTES],
    void *context)
{
    (void)context;
    if (public_key == NULL || signature == NULL ||
        (message == NULL && message_len != 0U)) {
        return false;
    }
    static const uint8_t k_empty_message = 0U;
    if (message == NULL) message = &k_empty_message;
    return ed25519_verify(signature, message, message_len, public_key) == 1;
}

static esp_err_t get_provider_config(airplay_identity_config_t *out_config,
                                     void *context)
{
    (void)context;
    if (out_config == NULL || !s_ready) return ESP_ERR_INVALID_STATE;
    memset(out_config, 0, sizeof(*out_config));
    if (s_info_plist_len == 0U) return ESP_ERR_INVALID_STATE;

    const size_t identifier_len = strlen(s_pairing_id);
    out_config->rtsp_server.pairing.pair_setup = (airplay_pair_setup_config_t){
        .pairing_code = s_pairing_code,
        .accessory_identifier = (const uint8_t *)s_pairing_id,
        .accessory_identifier_len = identifier_len,
        .accessory_public_key = s_public_key,
        .random = provider_random,
        .sign_accessory = sign_accessory,
        .verify_controller = verify_controller,
        .save_pairing = airplay_pairing_store_save_callback,
        .provider_context = NULL,
    };
    out_config->rtsp_server.pairing.pair_verify = (airplay_pair_verify_config_t){
        .accessory_identifier = (const uint8_t *)s_pairing_id,
        .accessory_identifier_len = identifier_len,
        .random = provider_random,
        .sign_accessory = sign_accessory,
        .verify_controller = verify_controller,
        .lookup_controller = airplay_pairing_store_lookup_callback,
        .provider_context = NULL,
    };
    out_config->rtsp_server.info_plist = s_info_plist;
    out_config->rtsp_server.info_plist_len = s_info_plist_len;
    out_config->rtsp_server.control_port = 7000U;
    out_config->rtsp_server.screen_port = 0U;
    out_config->mdns_hostname = s_mdns_hostname;
    out_config->mdns_instance_name = s_mdns_instance;
    out_config->mdns_txt_records = s_mdns_txt;
    out_config->mdns_txt_record_count = sizeof(s_mdns_txt) / sizeof(s_mdns_txt[0]);
    out_config->device_identifier = s_device_id;
    out_config->public_key = s_public_key_hex;
    out_config->source_version = AIRPLAY_SOURCE_VERSION;
    out_config->main_display_width_pixels = AIRPLAY_DISPLAY_WIDTH;
    out_config->main_display_height_pixels = AIRPLAY_DISPLAY_HEIGHT;
    out_config->main_display_rotation_degrees = 0U;
    return ESP_OK;
}

esp_err_t airplay_software_provider_init(void)
{
    if (s_ready) return ESP_OK;
    if (s_pairing_code[0] == '\0' ||
        strlen(s_pairing_code) > AIRPLAY_PAIR_SETUP_MAX_PAIRING_CODE_BYTES) {
        ESP_LOGE(TAG, "configured Pair-Setup code is empty or too long");
        return ESP_ERR_INVALID_ARG;
    }

    size_t paired_controller_count = 0U;
    esp_err_t err = airplay_pairing_store_count(&paired_controller_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "controller store is unavailable: %s", esp_err_to_name(err));
        return err;
    }

    err = load_or_create_identity();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "persistent AirPlay identity load failed: %s", esp_err_to_name(err));
        return err;
    }
    err = format_device_id();
    if (err != ESP_OK) return err;
    err = prepare_info_plist();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "embedded /info plist could not be prepared: %s", esp_err_to_name(err));
        return err;
    }

    const airplay_identity_provider_t provider = {
        .get_config = get_provider_config,
        .context = NULL,
    };
    err = airplay_identity_port_register_provider(&provider);
    if (err != ESP_OK) {
        memset(s_private_key, 0, sizeof(s_private_key));
        memset(s_info_plist, 0, sizeof(s_info_plist));
        s_info_plist_len = 0U;
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG,
             "software AirPlay identity ready id=%s deviceid=%s paired_controllers=%u; "
             "receiver seed is stored in NVS",
             s_pairing_id, s_device_id, (unsigned)paired_controller_count);
    return ESP_OK;
}
