#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_bt_device.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bt_port.h"
#include "auth_port.h"
#include "auth_software_provider.h"
#include "airplay_identity_port.h"
#include "airplay_software_provider.h"
#include "airplay_pairing_store.h"
#include "airplay_screen.h"
#include "airplay_rtsp_server.h"
#include "iap2_core.h"
#include "iap2_carplay.h"
#include "iap2_identity.h"
#include "iap2_mfi_auth.h"
#include "net_port.h"
#include "media_bridge.h"
#include "web_gateway.h"
#include "time_port.h"

#define IAP2_EVENT_QUEUE_DEPTH 8
#define IAP2_EVENT_DATA_BYTES 512
#define IAP2_WORKER_STACK_BYTES 12288
#define IAP2_IDENTIFICATION_MAX_RETRIES 4U
#define IAP2_PRE_TRANSPORT_WIFI_CONFIG_LIMIT 5
#define IAP2_POST_TRANSPORT_WIFI_CONFIG_LIMIT 2

typedef enum {
    IAP2_RUNTIME_CONNECTED,
    IAP2_RUNTIME_DISCONNECTED,
    IAP2_RUNTIME_RX,
} iap2_runtime_event_type_t;

typedef struct {
    iap2_runtime_event_type_t type;
    size_t len;
    uint8_t data[IAP2_EVENT_DATA_BYTES];
} iap2_runtime_event_t;

static const char *TAG = "app_main";
static QueueHandle_t s_iap2_events;
static iap2_core_t *s_iap2_core;
static iap2_mfi_auth_t s_iap2_mfi_auth;
static bool s_iap2_mfi_response_pending;
static bool s_iap2_start_session_sent;
static bool s_iap2_transport_notification_seen;
static uint8_t s_iap2_pre_transport_wifi_config_count;
static uint8_t s_iap2_post_transport_wifi_config_count;
static bool s_iap2_identification_accepted;
static bool s_iap2_subscriptions_sent;
static uint16_t s_iap2_omitted_message_ids[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
static size_t s_iap2_omitted_message_count;
static uint8_t s_iap2_identification_retry_count;
static uint8_t s_iap2_certificate_buffer[1536];
static uint8_t s_iap2_auth_body_buffer[sizeof(s_iap2_certificate_buffer) + 4U];
static bool s_airplay_identity_ready;
static airplay_identity_config_t s_airplay_identity;
static airplay_rtsp_server_t *s_airplay_server;
static const char *s_airplay_server_reason = "deployment_provider_unavailable";
static bool s_airplay_mdns_ready;

/* Optional deployment unit may override MFi auth and registers the AirPlay identity provider. */
extern esp_err_t app_register_deployment_providers(void) __attribute__((weak));

static uint64_t monotonic_ms(void);

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8);
    bytes[1] = (uint8_t)value;
}

static bool find_single_parameter(const uint8_t *body, size_t body_len, uint16_t wanted_id,
                                  const uint8_t **payload, size_t *payload_len)
{
    if (body == NULL || payload == NULL || payload_len == NULL) {
        return false;
    }
    size_t offset = 0;
    bool found = false;
    while (offset < body_len) {
        if (body_len - offset < 4) {
            return false;
        }
        uint16_t parameter_len = read_be16(body + offset);
        if (parameter_len < 4 || parameter_len > body_len - offset) {
            return false;
        }
        if (read_be16(body + offset + 2) == wanted_id) {
            if (found) {
                return false;
            }
            found = true;
            *payload = body + offset + 4;
            *payload_len = parameter_len - 4;
        }
        offset += parameter_len;
    }
    return found;
}

static bool validate_parameter_list(const uint8_t *body, size_t body_len)
{
    if (body == NULL && body_len != 0U) return false;
    size_t offset = 0U;
    while (offset < body_len) {
        if (body_len - offset < 4U) return false;
        uint16_t parameter_len = read_be16(body + offset);
        if (parameter_len < 4U || parameter_len > body_len - offset) return false;
        offset += parameter_len;
    }
    return true;
}

static void reset_iap2_wireless_negotiation(void)
{
    s_iap2_start_session_sent = false;
    s_iap2_transport_notification_seen = false;
    s_iap2_pre_transport_wifi_config_count = 0U;
    s_iap2_post_transport_wifi_config_count = 0U;
    s_iap2_subscriptions_sent = false;
}

static void reset_iap2_identification(void)
{
    memset(s_iap2_omitted_message_ids, 0, sizeof(s_iap2_omitted_message_ids));
    s_iap2_omitted_message_count = 0U;
    s_iap2_identification_retry_count = 0U;
    s_iap2_identification_accepted = false;
}

static void reset_iap2_connection_state(void)
{
    iap2_mfi_auth_reset(&s_iap2_mfi_auth);
    s_iap2_mfi_response_pending = false;
    reset_iap2_identification();
    reset_iap2_wireless_negotiation();
}

static size_t encode_bytes_parameter(uint8_t *out, size_t capacity, uint16_t id,
                                     const uint8_t *payload, size_t payload_len)
{
    if (out == NULL || (payload == NULL && payload_len != 0) ||
        payload_len > UINT16_MAX - 4U || payload_len + 4U > capacity) {
        return 0;
    }
    write_be16(out, (uint16_t)(payload_len + 4U));
    write_be16(out + 2, id);
    if (payload_len != 0) {
        memcpy(out + 4, payload, payload_len);
    }
    return payload_len + 4U;
}

static bool append_parameter(uint8_t *out, size_t capacity, size_t *used,
                             uint16_t id, const uint8_t *payload, size_t payload_len)
{
    if (used == NULL || *used > capacity) return false;
    size_t written = encode_bytes_parameter(out + *used, capacity - *used,
                                            id, payload, payload_len);
    if (written == 0) return false;
    *used += written;
    return true;
}

static bool append_void_parameter(uint8_t *out, size_t capacity, size_t *used, uint16_t id)
{
    return append_parameter(out, capacity, used, id, NULL, 0);
}

static bool send_iap2_csm_with_ack_callback(uint16_t message_id,
                                            const uint8_t *body,
                                            size_t body_len,
                                            iap2_core_csm_acked_cb_t acked_callback,
                                            void *acked_context)
{
    if (s_iap2_core == NULL ||
        !iap2_core_send_csm_with_ack_callback(s_iap2_core, message_id, body, body_len,
                                              acked_callback, acked_context, monotonic_ms())) {
        ESP_LOGW(TAG, "iap2_tx=deferred_or_rejected id=0x%04x bytes=%u",
                 message_id, (unsigned)body_len);
        return false;
    }
    return true;
}

static bool send_iap2_csm(uint16_t message_id, const uint8_t *body, size_t body_len)
{
    return send_iap2_csm_with_ack_callback(message_id, body, body_len, NULL, NULL);
}

static esp_err_t start_airplay_mdns(void)
{
    if (!s_airplay_identity_ready || s_airplay_server == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    uint16_t port = airplay_rtsp_server_get_port(s_airplay_server);
    if (port == 0U) return ESP_ERR_INVALID_STATE;

    esp_err_t err = mdns_init();
    if (err != ESP_OK) return err;
    err = mdns_hostname_set(s_airplay_identity.mdns_hostname);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    err = mdns_instance_name_set(s_airplay_identity.mdns_instance_name);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    mdns_txt_item_t txt[16];
    for (size_t i = 0U; i < s_airplay_identity.mdns_txt_record_count; ++i) {
        txt[i].key = s_airplay_identity.mdns_txt_records[i].key;
        txt[i].value = s_airplay_identity.mdns_txt_records[i].value;
    }
    err = mdns_service_add(s_airplay_identity.mdns_instance_name,
                           "_airplay", "_tcp", port,
                           txt, s_airplay_identity.mdns_txt_record_count);
    if (err != ESP_OK) {
        mdns_free();
        return err;
    }
    s_airplay_mdns_ready = true;
    ESP_LOGI(TAG, "airplay_mdns=advertising service=_airplay._tcp port=%u txt_count=%u",
             (unsigned)port, (unsigned)s_airplay_identity.mdns_txt_record_count);
    return ESP_OK;
}

static void on_airplay_event_state(bool connected, void *context)
{
    (void)context;
    if (connected) return;
    esp_err_t err = web_gateway_release_touches();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "touch_release=event_disconnect_queue_failed error=%s",
                 esp_err_to_name(err));
    }
}

static uint64_t monotonic_ms(void)
{
    return (uint64_t)time_port_monotonic_ms();
}

static bool iap2_tx(const uint8_t *bytes, size_t len, void *context)
{
    (void)context;
    return bt_port_send(bytes, len) == ESP_OK;
}

static bool send_identification_information(void)
{
    const uint8_t *mac = esp_bt_dev_get_address();
    if (mac == NULL || CONFIG_CARPLAY_AP_SSID[0] == '\0') {
        ESP_LOGE(TAG, "iap2_identification=unavailable bt_address_or_ssid_missing");
        return false;
    }
    char serial[13];
    snprintf(serial, sizeof(serial), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    const esp_app_desc_t *app = esp_app_get_description();
    iap2_identity_config_t identity = {
        .name = "ESP32 CarPlay Web Relay",
        .model_identifier = "ESP32-CP-WEB",
        .manufacturer = "ESP32 CarPlay Web Relay",
        .serial_number = serial,
        .firmware_version = app != NULL ? app->version : "unknown",
        .hardware_version = "ESP32",
        .language = "en",
        .wifi_ssid = CONFIG_CARPLAY_AP_SSID,
    };
    memcpy(identity.bluetooth_address, mac, sizeof(identity.bluetooth_address));
    uint8_t body[1024];
    size_t body_len = 0;
    if (!iap2_identity_build_wireless_omitting(&identity,
                                               s_iap2_omitted_message_ids,
                                               s_iap2_omitted_message_count,
                                               body, sizeof(body), &body_len) ||
        !send_iap2_csm(0x1d01, body, body_len)) {
        ESP_LOGE(TAG, "iap2_identification=send_failed");
        return false;
    }
    ESP_LOGI(TAG, "iap2_identification=information_sent omitted_messages=%u auth_provider=%s",
             (unsigned)s_iap2_omitted_message_count,
             auth_port_is_configured() ? "configured" : "not_configured");
    return true;
}

static void handle_start_identification(void)
{
    (void)send_identification_information();
}

static void handle_identification_rejected(const uint8_t *body, size_t body_len)
{
    uint16_t rejected_message_ids[IAP2_IDENTITY_MAX_OMITTED_MESSAGES];
    size_t rejected_message_count = 0U;
    if (!iap2_identity_parse_unsupported_messages(body, body_len,
                                                 rejected_message_ids,
                                                 IAP2_IDENTITY_MAX_OMITTED_MESSAGES,
                                                 &rejected_message_count)) {
        ESP_LOGE(TAG, "iap2_identification=rejected reason=unsupported_fields_not_retryable bytes=%u",
                 (unsigned)body_len);
        for (size_t i = 0; i < body_len; ++i) {
            ESP_LOGW(TAG, "iap2_identification=rejection_byte offset=%u value=0x%02x",
                     (unsigned)i, body[i]);
        }
        return;
    }

    size_t added_count = 0U;
    for (size_t i = 0; i < rejected_message_count; ++i) {
        const uint16_t message_id = rejected_message_ids[i];
        ESP_LOGW(TAG, "iap2_identification=unsupported_message id=0x%04x", message_id);
        bool already_omitted = false;
        for (size_t j = 0; j < s_iap2_omitted_message_count; ++j) {
            if (s_iap2_omitted_message_ids[j] == message_id) {
                already_omitted = true;
                break;
            }
        }
        if (already_omitted) continue;
        if (s_iap2_omitted_message_count >= IAP2_IDENTITY_MAX_OMITTED_MESSAGES) {
            ESP_LOGE(TAG, "iap2_identification=rejected reason=unsupported_message_limit_reached");
            return;
        }
        s_iap2_omitted_message_ids[s_iap2_omitted_message_count++] = message_id;
        ++added_count;
    }
    if (added_count == 0U) {
        ESP_LOGE(TAG, "iap2_identification=rejected reason=no_new_unsupported_messages retries=%u",
                 (unsigned)s_iap2_identification_retry_count);
        return;
    }
    if (s_iap2_identification_retry_count >= IAP2_IDENTIFICATION_MAX_RETRIES) {
        ESP_LOGE(TAG, "iap2_identification=rejected reason=retry_limit_reached retries=%u",
                 (unsigned)s_iap2_identification_retry_count);
        return;
    }
    ++s_iap2_identification_retry_count;
    ESP_LOGW(TAG, "iap2_identification=retry_without_unsupported_messages retry=%u added=%u total=%u",
             (unsigned)s_iap2_identification_retry_count, (unsigned)added_count,
             (unsigned)s_iap2_omitted_message_count);
    (void)send_identification_information();
}

static void handle_authentication_certificate(void)
{
    if (!auth_port_is_configured()) {
        ESP_LOGE(TAG, "iap2_mfi=certificate_unavailable auth_provider=not_configured");
        return;
    }
    size_t certificate_len = sizeof(s_iap2_certificate_buffer);
    esp_err_t err = auth_port_read_certificate(s_iap2_certificate_buffer, &certificate_len);
    if (err != ESP_OK || certificate_len == 0 ||
        certificate_len > sizeof(s_iap2_certificate_buffer)) {
        ESP_LOGE(TAG, "iap2_mfi=certificate_read_failed error=%s", esp_err_to_name(err));
        return;
    }
    size_t body_len = encode_bytes_parameter(s_iap2_auth_body_buffer,
                                             sizeof(s_iap2_auth_body_buffer), 0,
                                             s_iap2_certificate_buffer, certificate_len);
    if (body_len == 0 || !send_iap2_csm(0xaa01, s_iap2_auth_body_buffer, body_len)) {
        ESP_LOGE(TAG, "iap2_mfi=certificate_send_failed bytes=%u", (unsigned)certificate_len);
        return;
    }
    ESP_LOGI(TAG, "iap2_mfi=certificate_sent bytes=%u", (unsigned)certificate_len);
}

static void iap2_mfi_challenge_response_acked(uint16_t message_id, void *context)
{
    (void)context;
    s_iap2_mfi_response_pending = false;
    if (message_id != 0xaa03U) {
        iap2_mfi_auth_mark_failed(&s_iap2_mfi_auth);
        ESP_LOGE(TAG, "iap2_mfi=unexpected_tx_completion id=0x%04x", message_id);
        return;
    }
    if (!iap2_mfi_auth_mark_challenge_response_acked(&s_iap2_mfi_auth)) {
        ESP_LOGW(TAG, "iap2_mfi=stale_challenge_response_acked state=%s",
                 iap2_mfi_auth_stage_name(iap2_mfi_auth_stage(&s_iap2_mfi_auth)));
        return;
    }
    ESP_LOGI(TAG, "iap2_mfi=challenge_response_acked");
}

static bool handle_authentication_challenge(const uint8_t *body, size_t body_len)
{
    const uint8_t *challenge = NULL;
    size_t challenge_len = 0;
    if (!auth_port_is_configured() ||
        !find_single_parameter(body, body_len, 0, &challenge, &challenge_len) ||
        challenge_len == 0 || challenge_len > 128) {
        ESP_LOGE(TAG, "iap2_mfi=challenge_rejected provider=%d body_bytes=%u",
                 auth_port_is_configured(), (unsigned)body_len);
        return false;
    }
    uint8_t signature[512];
    size_t signature_len = sizeof(signature);
    esp_err_t err = auth_port_sign_challenge(challenge, challenge_len,
                                             signature, &signature_len);
    if (err != ESP_OK || signature_len == 0 || signature_len > sizeof(signature)) {
        ESP_LOGE(TAG, "iap2_mfi=challenge_sign_failed error=%s", esp_err_to_name(err));
        return false;
    }
    uint8_t response[sizeof(signature) + 4U];
    size_t response_len = encode_bytes_parameter(response, sizeof(response), 0,
                                                 signature, signature_len);
    if (response_len == 0 ||
        !send_iap2_csm_with_ack_callback(0xaa03, response, response_len,
                                         iap2_mfi_challenge_response_acked, NULL)) {
        ESP_LOGE(TAG, "iap2_mfi=challenge_response_send_failed");
        return false;
    }
    ESP_LOGI(TAG, "iap2_mfi=challenge_response_queued_for_link_ack bytes=%u",
             (unsigned)signature_len);
    return true;
}

static bool iap2_message_is_omitted(uint16_t message_id)
{
    for (size_t i = 0; i < s_iap2_omitted_message_count; ++i) {
        if (s_iap2_omitted_message_ids[i] == message_id) return true;
    }
    return false;
}

static void send_wireless_subscriptions(void)
{
    uint8_t body[64];
    uint8_t group0[32];
    uint8_t group1[16];
    size_t body_len = 0;
    size_t group0_len = 0;
    size_t group1_len = 0;
    const uint16_t now_playing_fields[] = {1, 4, 6, 12, 26};
    const uint16_t playback_fields[] = {0, 1, 7};
    for (size_t i = 0; i < sizeof(now_playing_fields) / sizeof(now_playing_fields[0]); ++i) {
        (void)append_void_parameter(group0, sizeof(group0), &group0_len, now_playing_fields[i]);
    }
    for (size_t i = 0; i < sizeof(playback_fields) / sizeof(playback_fields[0]); ++i) {
        (void)append_void_parameter(group1, sizeof(group1), &group1_len, playback_fields[i]);
    }
    (void)append_parameter(body, sizeof(body), &body_len, 0, group0, group0_len);
    (void)append_parameter(body, sizeof(body), &body_len, 1, group1, group1_len);
    if (!iap2_message_is_omitted(0x5000)) {
        (void)send_iap2_csm(0x5000, body, body_len);
    }

    body_len = 0;
    uint8_t route_id[2] = {0, 42};
    (void)append_parameter(body, sizeof(body), &body_len, 0, route_id, sizeof(route_id));
    (void)append_void_parameter(body, sizeof(body), &body_len, 1);
    (void)append_void_parameter(body, sizeof(body), &body_len, 2);
    if (!iap2_message_is_omitted(0x5200)) {
        (void)send_iap2_csm(0x5200, body, body_len);
    }

    static const uint16_t message_ids[] = {0xae00, 0x4157, 0x4154};
    static const uint8_t parameter_ids[][6] = {
        {4, 5, 6, 0, 0, 0},
        {0, 4, 5, 0, 0, 0},
        {0, 1, 2, 3, 4, 11},
    };
    static const uint8_t parameter_counts[] = {3, 3, 6};
    for (size_t i = 0; i < sizeof(message_ids) / sizeof(message_ids[0]); ++i) {
        if (iap2_message_is_omitted(message_ids[i])) {
            ESP_LOGW(TAG, "iap2_subscription=skipped reason=message_omitted id=0x%04x",
                     message_ids[i]);
            continue;
        }
        body_len = 0;
        for (size_t j = 0; j < parameter_counts[i]; ++j) {
            (void)append_void_parameter(body, sizeof(body), &body_len, parameter_ids[i][j]);
        }
        (void)send_iap2_csm(message_ids[i], body, body_len);
    }
}

static void maybe_send_wireless_subscriptions(void)
{
    if (s_iap2_subscriptions_sent || !s_iap2_identification_accepted ||
        !iap2_mfi_auth_is_authenticated(&s_iap2_mfi_auth)) {
        return;
    }
    send_wireless_subscriptions();
    s_iap2_subscriptions_sent = true;
    ESP_LOGI(TAG, "iap2_subscriptions=sent after=identification_and_mfi_authentication");
}

static bool send_wifi_configuration(bool post_transport)
{
    if (!iap2_mfi_auth_is_authenticated(&s_iap2_mfi_auth)) {
        ESP_LOGW(TAG, "iap2_wifi_configuration=refused authentication_required");
        return false;
    }
    uint8_t *sent_count = post_transport
                              ? &s_iap2_post_transport_wifi_config_count
                              : &s_iap2_pre_transport_wifi_config_count;
    uint8_t limit = post_transport ? IAP2_POST_TRANSPORT_WIFI_CONFIG_LIMIT
                                   : IAP2_PRE_TRANSPORT_WIFI_CONFIG_LIMIT;
    if (*sent_count >= limit) {
        ESP_LOGW(TAG, "iap2_wifi_configuration=ignored reason=send_limit_reached phase=%s",
                 post_transport ? "post_transport" : "pre_transport");
        return false;
    }
    if (CONFIG_CARPLAY_AP_SSID[0] == '\0') {
        ESP_LOGE(TAG, "iap2_wifi_configuration=unavailable local_softap_credentials_invalid");
        return false;
    }
    uint8_t bssid[6];
    if (esp_wifi_get_mac(WIFI_IF_AP, bssid) != ESP_OK) {
        ESP_LOGE(TAG, "iap2_wifi_configuration=unavailable softap_bssid_read_failed");
        return false;
    }
    uint8_t body[128];
    size_t body_len = 0;
    const iap2_carplay_wifi_config_t config = {
        .ssid = CONFIG_CARPLAY_AP_SSID,
        .passphrase = CONFIG_CARPLAY_AP_PASSWORD,
        .channel = CONFIG_CARPLAY_AP_CHANNEL,
        .security_type = 2, /* WPA/WPA2 */
        .bssid = bssid,
    };
    if (!iap2_carplay_build_wifi_configuration(&config, body, sizeof(body), &body_len)) {
        ESP_LOGE(TAG, "iap2_wifi_configuration=unavailable local_softap_config_invalid");
        return false;
    }
    if (send_iap2_csm(0x5703, body, body_len)) {
        ++*sent_count;
        ESP_LOGI(TAG, "iap2_wifi_configuration=sent ssid_bytes=%u channel=%u bssid=present",
                 (unsigned)strlen(CONFIG_CARPLAY_AP_SSID), CONFIG_CARPLAY_AP_CHANNEL);
        return true;
    }
    return false;
}

static void send_carplay_start_session(void)
{
    if (!iap2_mfi_auth_is_authenticated(&s_iap2_mfi_auth)) {
        ESP_LOGW(TAG, "iap2_carplay_start=refused authentication_required");
        return;
    }
    if (s_iap2_start_session_sent) {
        ESP_LOGI(TAG, "iap2_carplay_start=ignored duplicate_start_request");
        return;
    }
    uint16_t airplay_port = airplay_rtsp_server_get_port(s_airplay_server);
    if (!s_airplay_identity_ready || s_airplay_server == NULL || !s_airplay_mdns_ready ||
        airplay_port == 0U ||
        !airplay_rtsp_server_is_running()) {
        ESP_LOGW(TAG, "iap2_carplay_start=unavailable reason=%s",
                 s_airplay_mdns_ready ? "airplay_endpoint_not_ready" : "airplay_discovery_not_ready");
        return;
    }
    esp_netif_t *ap_netif = net_port_get_ap_netif();
    esp_netif_ip_info_t ap_ip;
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ap_ip) != ESP_OK ||
        ap_ip.ip.addr == 0U) {
        ESP_LOGE(TAG, "iap2_carplay_start=unavailable reason=softap_ip_unavailable");
        return;
    }
    char ap_ip_text[16];
    (void)snprintf(ap_ip_text, sizeof(ap_ip_text), IPSTR, IP2STR(&ap_ip.ip));
    const char *ip_addresses[] = {ap_ip_text};
    const iap2_carplay_start_session_config_t config = {
        .ssid = CONFIG_CARPLAY_AP_SSID,
        .passphrase = CONFIG_CARPLAY_AP_PASSWORD,
        .channel = CONFIG_CARPLAY_AP_CHANNEL,
        .security_type = 2, /* WPA/WPA2 */
        .ip_addresses = ip_addresses,
        .ip_address_count = sizeof(ip_addresses) / sizeof(ip_addresses[0]),
        .airplay_port = airplay_port,
        .device_identifier = s_airplay_identity.device_identifier,
        .public_key = s_airplay_identity.public_key,
        .source_version = s_airplay_identity.source_version,
    };
    uint8_t body[1200];
    size_t body_len = 0U;
    if (!iap2_carplay_build_start_session(&config, body, sizeof(body), &body_len)) {
        ESP_LOGE(TAG, "iap2_carplay_start=build_failed reason=endpoint_config_invalid");
        return;
    }
    if (!send_iap2_csm(0x4301, body, body_len)) {
        ESP_LOGE(TAG, "iap2_carplay_start=send_failed bytes=%u", (unsigned)body_len);
        return;
    }
    s_iap2_start_session_sent = true;
    ESP_LOGI(TAG, "iap2_carplay_start=sent airplay_port=%u ip=" IPSTR,
             (unsigned)airplay_port, IP2STR(&ap_ip.ip));
}

static void iap2_message(uint16_t message_id, const uint8_t *body, size_t body_len, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "iap2_control_rx id=0x%04x bytes=%u", message_id, (unsigned)body_len);
    switch (message_id) {
    case 0x1d00:
        handle_start_identification();
        break;
    case 0x1d02:
        s_iap2_identification_accepted = true;
        ESP_LOGI(TAG, "iap2_identification=accepted");
        maybe_send_wireless_subscriptions();
        break;
    case 0x1d03:
        handle_identification_rejected(body, body_len);
        break;
    case 0xaa00:
        iap2_mfi_auth_reset(&s_iap2_mfi_auth);
        /* An older queued 0xaa03 still owns the link ACK callback; keep challenges gated. */
        reset_iap2_wireless_negotiation();
        handle_authentication_certificate();
        break;
    case 0xaa02:
        if (s_iap2_mfi_response_pending ||
            !iap2_mfi_auth_can_respond_to_challenge(&s_iap2_mfi_auth)) {
            ESP_LOGE(TAG, "iap2_mfi=challenge_rejected state=%s",
                     iap2_mfi_auth_stage_name(iap2_mfi_auth_stage(&s_iap2_mfi_auth)));
        } else if (!iap2_mfi_auth_mark_challenge_response_queued(&s_iap2_mfi_auth)) {
            ESP_LOGE(TAG, "iap2_mfi=challenge_state_update_failed");
        } else {
            s_iap2_mfi_response_pending = true;
            if (!handle_authentication_challenge(body, body_len)) {
                s_iap2_mfi_response_pending = false;
                iap2_mfi_auth_mark_failed(&s_iap2_mfi_auth);
            }
        }
        break;
    case 0xaa04:
        iap2_mfi_auth_mark_failed(&s_iap2_mfi_auth);
        reset_iap2_wireless_negotiation();
        ESP_LOGE(TAG, "iap2_mfi=authentication_failed");
        break;
    case 0xaa05:
        if (!iap2_mfi_auth_mark_authenticated(&s_iap2_mfi_auth)) {
            ESP_LOGE(TAG, "iap2_mfi=authentication_success_rejected reason=challenge_response_missing state=%s",
                     iap2_mfi_auth_stage_name(iap2_mfi_auth_stage(&s_iap2_mfi_auth)));
        } else {
            ESP_LOGI(TAG, "iap2_mfi=authentication_succeeded");
            maybe_send_wireless_subscriptions();
        }
        break;
    case 0x5702:
        (void)send_wifi_configuration(s_iap2_transport_notification_seen);
        break;
    case 0x4300:
        ESP_LOGI(TAG, "iap2_carplay_availability=received bytes=%u", (unsigned)body_len);
        send_carplay_start_session();
        break;
    case 0x4e0d: {
        const uint8_t *availability = NULL;
        size_t availability_len = 0U;
        if (!find_single_parameter(body, body_len, 0, &availability, &availability_len) ||
            availability_len != 1U || availability[0] > 1U) {
            ESP_LOGW(TAG, "iap2_wireless_carplay=malformed_update bytes=%u",
                     (unsigned)body_len);
        } else {
            ESP_LOGI(TAG, "iap2_wireless_carplay=availability_changed available=%u",
                     availability[0]);
        }
        break;
    }
    case 0x4e0e: {
        const uint8_t *bluetooth_identifier = NULL;
        const uint8_t *usb_identifier = NULL;
        size_t bluetooth_identifier_len = 0U;
        size_t usb_identifier_len = 0U;
        if (!validate_parameter_list(body, body_len)) {
            ESP_LOGW(TAG, "iap2_transport_notification=malformed bytes=%u",
                     (unsigned)body_len);
            break;
        }
        bool bluetooth_identifier_present =
            find_single_parameter(body, body_len, 0, &bluetooth_identifier,
                                  &bluetooth_identifier_len);
        bool usb_identifier_present =
            find_single_parameter(body, body_len, 1, &usb_identifier, &usb_identifier_len);
        (void)bluetooth_identifier;
        (void)usb_identifier;
        s_iap2_transport_notification_seen = true;
        ESP_LOGI(TAG, "iap2_transport_notification=received bluetooth_id=%s usb_id=%s",
                 bluetooth_identifier_present ? "present" : "absent",
                 usb_identifier_present ? "present" : "absent");
        (void)send_wifi_configuration(true);
        break;
    }
    default:
        break;
    }
}

static void iap2_state(iap2_core_state_t state, const char *reason, void *context)
{
    (void)context;
    if (state == IAP2_CORE_DEAD) {
        reset_iap2_connection_state();
    }
    ESP_LOGI(TAG, "iap2_state=%d reason=%s", (int)state, reason != NULL ? reason : "none");
}

static void enqueue_runtime_event(const iap2_runtime_event_t *event)
{
    if (event == NULL || s_iap2_events == NULL) {
        ESP_LOGE(TAG, "iap2_runtime=unavailable event=%d", event != NULL ? event->type : -1);
        return;
    }
    if (xQueueSend(s_iap2_events, event, 0) == pdPASS) return;

    if (event->type == IAP2_RUNTIME_RX) {
        ESP_LOGE(TAG, "iap2_runtime=rx_overflow action=disconnect_session bytes=%u",
                 (unsigned)event->len);
        (void)bt_port_disconnect();
        return;
    }

    /* Preserve connection boundaries even if the consumer temporarily fell behind. */
    ESP_LOGW(TAG, "iap2_runtime=state_queue_full event=%d action=discard_stale_rx",
             event->type);
    (void)xQueueReset(s_iap2_events);
    if (xQueueSend(s_iap2_events, event, 0) != pdPASS) {
        ESP_LOGE(TAG, "iap2_runtime=state_unavailable event=%d", event->type);
    }
}

static void bt_rx(const uint8_t *data, size_t len, void *context)
{
    (void)context;
    if (data == NULL || len == 0 || len > IAP2_EVENT_DATA_BYTES) {
        ESP_LOGW(TAG, "rfcomm_rx=invalid_length bytes=%u", (unsigned)len);
        return;
    }
    iap2_runtime_event_t event = {.type = IAP2_RUNTIME_RX, .len = len};
    memcpy(event.data, data, len);
    enqueue_runtime_event(&event);
}

static void bt_state(bool connected, void *context)
{
    (void)context;
    iap2_runtime_event_t event = {
        .type = connected ? IAP2_RUNTIME_CONNECTED : IAP2_RUNTIME_DISCONNECTED,
    };
    enqueue_runtime_event(&event);
}

static void iap2_worker(void *argument)
{
    (void)argument;
    iap2_core_t *core = iap2_core_create(iap2_tx, iap2_message, iap2_state, NULL);
    if (core == NULL) {
        ESP_LOGE(TAG, "iap2_core=create_failed");
        vTaskDelete(NULL);
        return;
    }
    s_iap2_core = core;

    iap2_runtime_event_t event;
    while (true) {
        /* The browser must finish authenticated WebRTC setup before we invite
         * the phone to start CarPlay. Reuse this worker rather than another task. */
        bt_port_set_discovery_enabled(media_bridge_is_connected());
        if (xQueueReceive(s_iap2_events, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            switch (event.type) {
            case IAP2_RUNTIME_CONNECTED:
                reset_iap2_connection_state();
                iap2_core_reset(core, "rfcomm_reconnect");
                ESP_LOGI(TAG, "rfcomm=starting_iap2_link");
                (void)iap2_core_start(core, monotonic_ms());
                break;
            case IAP2_RUNTIME_DISCONNECTED:
                reset_iap2_connection_state();
                iap2_core_reset(core, "rfcomm_disconnected");
                break;
            case IAP2_RUNTIME_RX:
                iap2_core_feed(core, event.data, event.len, monotonic_ms());
                break;
            }
        }
        iap2_core_advance_time(core, monotonic_ms());
    }
}

void app_main(void)
{
    bool pairing_store_ready = false;
    if (!airplay_screen_crypto_self_test()) {
        ESP_LOGE(TAG, "airplay_crypto=self_test_failed chacha20_poly1305_unavailable");
        return;
    }
    ESP_LOGI(TAG, "airplay_crypto=self_test_passed algorithm=chacha20_poly1305");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition needs reinitialization; erasing it");
        err = nvs_flash_erase();
        if (err == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS initialization failed: %s", esp_err_to_name(err));
        return;
    }

    err = airplay_pairing_store_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "airplay_pairing_store=unavailable error=%s",
                 esp_err_to_name(err));
    } else {
        pairing_store_ready = true;
        ESP_LOGI(TAG, "airplay_pairing_store=ready max_controllers=%u",
                 (unsigned)AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS);
    }

    err = auth_software_provider_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "software MFi provider initialization failed: %s",
                 esp_err_to_name(err));
    }

    if (app_register_deployment_providers != NULL) {
        err = app_register_deployment_providers();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "deployment_provider=registration_failed error=%s",
                     esp_err_to_name(err));
        }
    }
    if (!airplay_identity_port_is_configured() && pairing_store_ready) {
        err = airplay_software_provider_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "software AirPlay provider initialization failed: %s",
                     esp_err_to_name(err));
        }
    }
    if (airplay_identity_port_is_configured()) {
        err = airplay_identity_port_get_config(&s_airplay_identity);
        if (err == ESP_OK) {
            s_airplay_identity_ready = true;
        } else {
            ESP_LOGW(TAG, "airplay_identity=provider_invalid error=%s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "airplay_identity=provider_unavailable rtsp_listener_will_not_start");
    }

    err = net_port_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP startup failed: %s", esp_err_to_name(err));
        return;
    }

    if (s_airplay_identity_ready) {
        err = web_gateway_set_display_geometry(
            s_airplay_identity.main_display_width_pixels,
            s_airplay_identity.main_display_height_pixels,
            s_airplay_identity.main_display_rotation_degrees);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "touch_display=config_failed error=%s", esp_err_to_name(err));
            return;
        }
    }

    err = web_gateway_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server startup failed: %s", esp_err_to_name(err));
        return;
    }

    if (!auth_port_is_configured()) {
        s_airplay_server_reason = "mfi_provider_unavailable";
        ESP_LOGW(TAG, "airplay_rtsp_server=not_started reason=%s", s_airplay_server_reason);
    } else if (!s_airplay_identity_ready) {
        s_airplay_server_reason = airplay_identity_port_is_configured()
                                      ? "airplay_identity_provider_invalid"
                                      : "airplay_identity_provider_unavailable";
        ESP_LOGW(TAG, "airplay_rtsp_server=not_started reason=%s", s_airplay_server_reason);
    } else {
        airplay_rtsp_server_config_t rtsp_config = s_airplay_identity.rtsp_server;
        rtsp_config.on_event_state = on_airplay_event_state;
        rtsp_config.event_state_context = NULL;
        err = airplay_rtsp_server_start(&rtsp_config, &s_airplay_server);
        if (err != ESP_OK) {
            s_airplay_server_reason = esp_err_to_name(err);
            ESP_LOGE(TAG, "airplay_rtsp_server=start_failed error=%s",
                     esp_err_to_name(err));
        } else {
            s_airplay_server_reason = "configured";
            err = start_airplay_mdns();
            if (err != ESP_OK) {
                s_airplay_server_reason = "mdns_start_failed";
                ESP_LOGE(TAG, "airplay_mdns=start_failed error=%s", esp_err_to_name(err));
            }
        }
    }

    s_iap2_events = xQueueCreate(IAP2_EVENT_QUEUE_DEPTH, sizeof(iap2_runtime_event_t));
    if (s_iap2_events == NULL ||
        xTaskCreate(iap2_worker, "iap2_worker", IAP2_WORKER_STACK_BYTES,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "iap2_worker=start_failed");
        return;
    }

    err = bt_port_start(bt_rx, bt_state, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "BR/EDR iAP2 bootstrap failed: %s", esp_err_to_name(err));
        return;
    }

    bool airplay_running = airplay_rtsp_server_is_running();
    ESP_LOGI(TAG, "airplay_rtsp_server=%s reason=%s",
             airplay_running ? "running" : "not_started", s_airplay_server_reason);
    ESP_LOGI(TAG, "startup=waiting_for_browser open=http://192.168.4.1/ pairing_code=11111111 phone_discovery=after_webrtc_connected");
}
