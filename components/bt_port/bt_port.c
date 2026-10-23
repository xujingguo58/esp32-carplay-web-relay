#include "bt_port.h"

#include <ctype.h>
#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_spp_api.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BT_PORT_DEVICE_NAME "ESP32-CarPlay"
#define BT_PORT_IAP2_UUID_BYTES { \
    0x00, 0x00, 0x00, 0x00, 0xde, 0xca, 0xfa, 0xde, \
    0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xfe \
}
#define BT_PORT_DISCOVERY_INQ_LEN 0x08
#define BT_PORT_MAX_PHONE_CANDIDATES 8
#define BT_PORT_DEVICE_NAME_MAX 64
#define BT_PORT_DEBUG_PIN "11111111"
#define BT_PORT_DISCOVERY_RETRY_US (5LL * 1000LL * 1000LL)
#define BT_PORT_RX_CHUNK_BYTES 512
#define BT_PORT_RX_QUEUE_DEPTH 8
#define BT_PORT_RX_TASK_STACK_BYTES 4096

typedef struct {
    esp_bd_addr_t address;
    uint32_t cod;
    int8_t rssi;
    char name[BT_PORT_DEVICE_NAME_MAX];
} bt_phone_candidate_t;

typedef enum {
    BT_RX_EVENT_DATA,
    BT_RX_EVENT_STATE,
} bt_rx_event_type_t;

typedef struct {
    bt_rx_event_type_t type;
    bool connected;
    uint16_t len;
    uint8_t data[BT_PORT_RX_CHUNK_BYTES];
} bt_rx_event_t;

static const char *TAG = "bt_port";
static QueueHandle_t s_rx_queue;
static bt_port_rx_cb_t s_rx_cb;
static bt_port_state_cb_t s_state_cb;
static void *s_callback_context;
static portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_started;
static bool s_connected;
static uint32_t s_spp_handle;
static uint32_t s_rx_dropped;
static bool s_sdp_ready;
static bool s_spp_ready;
static volatile bool s_discovery_enabled;
static bool s_discovery_started;
static bool s_discovery_retry_scheduled;
static bool s_sdp_search_pending;
static bool s_spp_connect_pending;
static esp_bd_addr_t s_target_address;
static bool s_target_valid;
static bt_phone_candidate_t s_phone_candidates[BT_PORT_MAX_PHONE_CANDIDATES];
static size_t s_phone_candidate_count;
static esp_timer_handle_t s_discovery_retry_timer;

static void begin_peer_discovery(void);
static void begin_iap2_service_search(const esp_bd_addr_t address);

static void cancel_discovery_retry(void)
{
    if (s_discovery_retry_timer != NULL && s_discovery_retry_scheduled) {
        (void)esp_timer_stop(s_discovery_retry_timer);
        s_discovery_retry_scheduled = false;
    }
}

static void schedule_discovery_retry(const char *reason)
{
    if (!s_discovery_enabled || s_discovery_retry_timer == NULL || s_connected || s_target_valid ||
        s_discovery_retry_scheduled) {
        return;
    }
    esp_err_t err = esp_timer_start_once(s_discovery_retry_timer,
                                          BT_PORT_DISCOVERY_RETRY_US);
    if (err == ESP_OK) {
        s_discovery_retry_scheduled = true;
        ESP_LOGI(TAG, "discovery=retry_scheduled delay_ms=%lld reason=%s",
                 BT_PORT_DISCOVERY_RETRY_US / 1000LL, reason);
    } else {
        ESP_LOGE(TAG, "discovery=retry_schedule_failed reason=%s error=%s",
                 reason, esp_err_to_name(err));
    }
}

static void reset_target_and_retry(const char *reason)
{
    s_target_valid = false;
    s_sdp_search_pending = false;
    s_spp_connect_pending = false;
    memset(s_target_address, 0, sizeof(s_target_address));
    schedule_discovery_retry(reason);
}

static void discovery_retry_callback(void *argument)
{
    (void)argument;
    s_discovery_retry_scheduled = false;
    if (!s_discovery_enabled) return;
    ESP_LOGI(TAG, "discovery=retry reason=previous_attempt_did_not_connect");
    begin_peer_discovery();
}

static void log_bt_address(const char *label, const esp_bd_addr_t address)
{
    ESP_LOGI(TAG, "%s=%02x:%02x:%02x:%02x:%02x:%02x", label,
             address[0], address[1], address[2], address[3], address[4], address[5]);
}

static bool address_equal(const esp_bd_addr_t lhs, const esp_bd_addr_t rhs)
{
    return memcmp(lhs, rhs, ESP_BD_ADDR_LEN) == 0;
}

static bool name_contains_iphone(const char *name)
{
    static const char needle[] = "iphone";
    if (name == NULL) return false;
    for (const char *start = name; *start != '\0'; ++start) {
        size_t i = 0;
        while (needle[i] != '\0' && start[i] != '\0' &&
               tolower((unsigned char)start[i]) == needle[i]) {
            ++i;
        }
        if (needle[i] == '\0') return true;
    }
    return false;
}

static void copy_remote_name(bt_phone_candidate_t *candidate,
                             const esp_bt_gap_dev_prop_t *property)
{
    if (property->type == ESP_BT_GAP_DEV_PROP_BDNAME && property->len > 0) {
        size_t length = (size_t)property->len;
        if (length >= sizeof(candidate->name)) length = sizeof(candidate->name) - 1;
        memcpy(candidate->name, property->val, length);
        candidate->name[length] = '\0';
        return;
    }

    if (property->type != ESP_BT_GAP_DEV_PROP_EIR || candidate->name[0] != '\0') return;
    uint8_t name_len = 0;
    uint8_t *name = esp_bt_gap_resolve_eir_data(property->val,
                                                 ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME,
                                                 &name_len);
    if (name == NULL) {
        name = esp_bt_gap_resolve_eir_data(property->val,
                                          ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME,
                                          &name_len);
    }
    if (name == NULL || name_len == 0) return;
    size_t length = name_len;
    if (length >= sizeof(candidate->name)) length = sizeof(candidate->name) - 1;
    memcpy(candidate->name, name, length);
    candidate->name[length] = '\0';
}

static void maybe_add_phone_candidate(const esp_bt_gap_cb_param_t *param)
{
    bool has_cod = false;
    uint32_t cod = 0;
    int8_t rssi = INT8_MIN;
    bt_phone_candidate_t candidate = {0};
    memcpy(candidate.address, param->disc_res.bda, ESP_BD_ADDR_LEN);

    for (int i = 0; i < param->disc_res.num_prop; ++i) {
        const esp_bt_gap_dev_prop_t *property = &param->disc_res.prop[i];
        if (property->type == ESP_BT_GAP_DEV_PROP_COD && property->val != NULL &&
            property->len >= (int)sizeof(cod)) {
            memcpy(&cod, property->val, sizeof(cod));
            has_cod = true;
        } else if (property->type == ESP_BT_GAP_DEV_PROP_RSSI &&
                   property->val != NULL && property->len >= (int)sizeof(rssi)) {
            memcpy(&rssi, property->val, sizeof(rssi));
        }
        copy_remote_name(&candidate, property);
    }

    if (!has_cod || ((cod >> 8) & 0x1fU) != ESP_BT_COD_MAJOR_DEV_PHONE) return;
    candidate.cod = cod;
    candidate.rssi = rssi;

    for (size_t i = 0; i < s_phone_candidate_count; ++i) {
        if (address_equal(s_phone_candidates[i].address, candidate.address)) {
            if (candidate.name[0] != '\0') {
                memcpy(s_phone_candidates[i].name, candidate.name, sizeof(candidate.name));
            }
            s_phone_candidates[i].cod = cod;
            s_phone_candidates[i].rssi = rssi;
            return;
        }
    }
    if (s_phone_candidate_count >= BT_PORT_MAX_PHONE_CANDIDATES) {
        ESP_LOGW(TAG, "discovery=phone_candidate_limit_reached limit=%u",
                 (unsigned)BT_PORT_MAX_PHONE_CANDIDATES);
        return;
    }
    s_phone_candidates[s_phone_candidate_count++] = candidate;
    ESP_LOGI(TAG, "discovery=phone_candidate name=%s cod=0x%06" PRIx32
             " rssi=%d count=%u", candidate.name[0] != '\0' ? candidate.name : "(unknown)",
             candidate.cod, candidate.rssi, (unsigned)s_phone_candidate_count);
    log_bt_address("discovery_candidate_address", candidate.address);
}

static void select_discovered_phone(void)
{
    size_t iphone_index = 0;
    size_t iphone_count = 0;
    for (size_t i = 0; i < s_phone_candidate_count; ++i) {
        if (name_contains_iphone(s_phone_candidates[i].name)) {
            iphone_index = i;
            ++iphone_count;
        }
    }

    size_t selected_index = 0;
    if (iphone_count == 1) {
        selected_index = iphone_index;
    } else if (iphone_count > 1) {
        ESP_LOGE(TAG, "discovery=ambiguous_iphone_candidates count=%u action=leave_unconnected",
                 (unsigned)iphone_count);
        return;
    } else if (s_phone_candidate_count == 1) {
        selected_index = 0;
    } else if (s_phone_candidate_count == 0) {
        ESP_LOGW(TAG, "discovery=no_phone_candidates; keep iPhone Settings > Bluetooth open");
        schedule_discovery_retry("no_phone_candidates");
        return;
    } else {
        ESP_LOGE(TAG, "discovery=ambiguous_phone_candidates count=%u; leave only target iPhone discoverable",
                 (unsigned)s_phone_candidate_count);
        return;
    }

    const bt_phone_candidate_t *selected = &s_phone_candidates[selected_index];
    cancel_discovery_retry();
    memcpy(s_target_address, selected->address, ESP_BD_ADDR_LEN);
    s_target_valid = true;
    ESP_LOGI(TAG, "target=selected name=%s cod=0x%06" PRIx32 " rssi=%d",
             selected->name[0] != '\0' ? selected->name : "(unknown)",
             selected->cod, selected->rssi);
    log_bt_address("target_address", s_target_address);
    begin_iap2_service_search(s_target_address);
}

static void begin_peer_discovery(void)
{
    if (!s_discovery_enabled || !s_sdp_ready || !s_spp_ready ||
        s_discovery_started || s_target_valid || s_connected ||
        s_sdp_search_pending || s_spp_connect_pending) return;

    s_phone_candidate_count = 0;
    memset(s_phone_candidates, 0, sizeof(s_phone_candidates));
    s_discovery_started = true;
    ESP_LOGI(TAG, "discovery=starting transport=BR_EDR inquiry_len=%u*1.28s target=iPhone",
             (unsigned)BT_PORT_DISCOVERY_INQ_LEN);
    esp_err_t err = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY,
                                                BT_PORT_DISCOVERY_INQ_LEN, 0);
    if (err != ESP_OK) {
        s_discovery_started = false;
        ESP_LOGE(TAG, "discovery=start_failed error=%s", esp_err_to_name(err));
        schedule_discovery_retry("inquiry_start_failed");
    }
}

static void begin_iap2_service_search(const esp_bd_addr_t address)
{
    if (!s_discovery_enabled || !s_sdp_ready || s_sdp_search_pending ||
        s_spp_connect_pending || s_connected) return;
    esp_bt_uuid_t uuid = {
        .len = ESP_UUID_LEN_128,
    };
    static const uint8_t iap2_uuid[ESP_UUID_LEN_128] = BT_PORT_IAP2_UUID_BYTES;
    memcpy(uuid.uuid.uuid128, iap2_uuid, sizeof(iap2_uuid));
    s_sdp_search_pending = true;
    ESP_LOGI(TAG, "iap2_sdp=searching uuid=00000000-deca-fade-deca-deafdecacafe");
    log_bt_address("iap2_sdp_target", address);
    esp_bd_addr_t remote_address;
    memcpy(remote_address, address, ESP_BD_ADDR_LEN);
    esp_err_t err = esp_sdp_search_record(remote_address, uuid);
    if (err != ESP_OK) {
        s_sdp_search_pending = false;
        ESP_LOGE(TAG, "iap2_sdp=search_start_failed error=%s", esp_err_to_name(err));
        reset_target_and_retry("sdp_search_start_failed");
    }
}

static void post_state(bool connected)
{
    bt_rx_event_t event = {
        .type = BT_RX_EVENT_STATE,
        .connected = connected,
    };
    if (s_rx_queue == NULL) return;
    if (xQueueSend(s_rx_queue, &event, 0) != pdPASS) {
        /* A state boundary is more important than buffered bytes from an old link. */
        (void)xQueueReset(s_rx_queue);
        if (xQueueSend(s_rx_queue, &event, 0) != pdPASS) {
            ESP_LOGE(TAG, "event_queue=state_unavailable connected=%d", connected);
        }
    }
}

static void set_connected(bool connected, uint32_t handle)
{
    bool changed;
    portENTER_CRITICAL(&s_state_mux);
    changed = s_connected != connected;
    s_connected = connected;
    if (connected) {
        s_spp_handle = handle;
    } else {
        s_spp_handle = 0;
    }
    portEXIT_CRITICAL(&s_state_mux);
    if (changed) {
        post_state(connected);
    }
}

static void rx_dispatch_task(void *argument)
{
    (void)argument;
    bt_rx_event_t event;
    while (true) {
        if (xQueueReceive(s_rx_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (event.type == BT_RX_EVENT_STATE) {
            if (s_state_cb != NULL) {
                s_state_cb(event.connected, s_callback_context);
            }
        } else if (event.type == BT_RX_EVENT_DATA && s_rx_cb != NULL) {
            s_rx_cb(event.data, event.len, s_callback_context);
        }
    }
}

static void gap_callback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT:
        if (s_discovery_started) maybe_add_phone_candidate(param);
        break;
    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED && s_discovery_started) {
            s_discovery_started = false;
            ESP_LOGI(TAG, "discovery=complete phone_candidates=%u",
                     (unsigned)s_phone_candidate_count);
            if (s_discovery_enabled) {
                select_discovered_phone();
            } else {
                ESP_LOGI(TAG, "discovery=paused reason=browser_not_ready");
            }
        }
        break;
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing_complete status=%d name=%s", param->auth_cmpl.stat,
                 (const char *)param->auth_cmpl.device_name);
        log_bt_address("pairing_peer", param->auth_cmpl.bda);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "pairing_confirmation=just_works_accept");
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {0};
        if (param->pin_req.min_16_digit) {
            ESP_LOGE(TAG, "pairing_request=pin_rejected reason=peer_requires_16_digits configured_pin_length=8");
            (void)esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin_code);
        } else {
            memcpy(pin_code, BT_PORT_DEBUG_PIN, sizeof(BT_PORT_DEBUG_PIN) - 1);
            ESP_LOGI(TAG, "pairing_request=legacy_pin_reply pin=11111111");
            (void)esp_bt_gap_pin_reply(param->pin_req.bda, true,
                                       sizeof(BT_PORT_DEBUG_PIN) - 1, pin_code);
        }
        break;
    }
    case ESP_BT_GAP_KEY_REQ_EVT:
        ESP_LOGI(TAG, "pairing_request=ssp_passkey_reply pin=11111111");
        (void)esp_bt_gap_ssp_passkey_reply(param->key_req.bda, true, 11111111);
        break;
    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "pairing_request=ssp_passkey_notification value=%06" PRIu32,
                 param->key_notif.passkey);
        break;
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "radio_mode=changed mode=0x%02x", param->mode_chg.mode);
        break;
    default:
        ESP_LOGD(TAG, "gap_event=%d", event);
        break;
    }
}

static void sdp_callback(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        if (param->init.status == ESP_SDP_SUCCESS) {
            s_sdp_ready = true;
            ESP_LOGI(TAG, "sdp=ready role=client");
            begin_peer_discovery();
        } else {
            ESP_LOGE(TAG, "sdp_init=failed status=%d", param->init.status);
        }
        break;
    case ESP_SDP_SEARCH_COMP_EVT: {
        s_sdp_search_pending = false;
        if (!s_target_valid || !address_equal(param->search.remote_addr, s_target_address)) {
            ESP_LOGW(TAG, "iap2_sdp=search_result_ignored reason=target_mismatch");
            break;
        }
        if (!s_discovery_enabled) {
            reset_target_and_retry("browser_not_ready");
            ESP_LOGI(TAG, "iap2_sdp=paused reason=browser_not_ready");
            break;
        }
        if (param->search.status != ESP_SDP_SUCCESS || param->search.record_count <= 0 ||
            param->search.records == NULL) {
            ESP_LOGE(TAG, "iap2_sdp=not_found status=%d records=%d",
                     param->search.status, param->search.record_count);
            reset_target_and_retry("iap2_service_not_found");
            break;
        }

        const esp_bluetooth_sdp_raw_record_t *record =
            (const esp_bluetooth_sdp_raw_record_t *)param->search.records;
        const int32_t channel = record->hdr.rfcomm_channel_number;
        if (channel < 1 || channel > 30) {
            ESP_LOGE(TAG, "iap2_sdp=invalid_rfcomm_channel channel=%" PRId32, channel);
            reset_target_and_retry("invalid_rfcomm_channel");
            break;
        }

        ESP_LOGI(TAG, "iap2_sdp=found records=%d rfcomm_channel=%" PRId32,
                 param->search.record_count, channel);
        s_spp_connect_pending = true;
        esp_err_t err = esp_spp_connect(ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT,
                                        ESP_SPP_ROLE_MASTER, (uint8_t)channel,
                                        s_target_address);
        if (err != ESP_OK) {
            s_spp_connect_pending = false;
            ESP_LOGE(TAG, "rfcomm=connect_start_failed error=%s", esp_err_to_name(err));
            reset_target_and_retry("rfcomm_connect_start_failed");
        } else {
            ESP_LOGI(TAG, "rfcomm=connecting role=client security=authenticate_encrypt");
        }
        break;
    }
    case ESP_SDP_DEINIT_EVT:
        ESP_LOGI(TAG, "sdp_deinit status=%d", param->deinit.status);
        break;
    default:
        ESP_LOGD(TAG, "sdp_event=%d", event);
        break;
    }
}

static void spp_callback(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status != ESP_SPP_SUCCESS) {
            ESP_LOGE(TAG, "spp_init=failed status=%d", param->init.status);
            return;
        }
        s_spp_ready = true;
        ESP_LOGI(TAG, "spp_init=ready mode=callback role=client");
        begin_peer_discovery();
        break;
    case ESP_SPP_CL_INIT_EVT:
        ESP_LOGI(TAG, "rfcomm=client_init status=%d", param->cl_init.status);
        if (param->cl_init.status != ESP_SPP_SUCCESS) {
            s_spp_connect_pending = false;
            reset_target_and_retry("rfcomm_client_init_failed");
        }
        break;
    case ESP_SPP_OPEN_EVT:
        s_spp_connect_pending = false;
        if (param->open.status == ESP_SPP_SUCCESS) {
            if (!s_discovery_enabled) {
                ESP_LOGI(TAG, "rfcomm=disconnecting reason=browser_not_ready");
                (void)esp_spp_disconnect(param->open.handle);
                break;
            }
            ESP_LOGI(TAG, "rfcomm=connected role=client handle=%" PRIu32,
                     param->open.handle);
            set_connected(true, param->open.handle);
        } else {
            ESP_LOGE(TAG, "rfcomm=connect_failed status=%d", param->open.status);
            reset_target_and_retry("rfcomm_connect_failed");
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        ESP_LOGW(TAG, "rfcomm=unexpected_server_open status=%d", param->srv_open.status);
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "rfcomm=closed status=%d", param->close.status);
        s_spp_connect_pending = false;
        if (s_rx_queue != NULL) {
            (void)xQueueReset(s_rx_queue);
        }
        set_connected(false, 0);
        reset_target_and_retry("rfcomm_closed");
        break;
    case ESP_SPP_DATA_IND_EVT: {
        if (param->data_ind.status != ESP_SPP_SUCCESS || param->data_ind.data == NULL) {
            ESP_LOGW(TAG, "rfcomm_rx=invalid status=%d", param->data_ind.status);
            return;
        }
        if (!bt_port_is_connected() || s_rx_queue == NULL) {
            ESP_LOGD(TAG, "rfcomm_rx=ignored reason=session_not_active");
            return;
        }
        size_t offset = 0;
        while (offset < param->data_ind.len) {
            bt_rx_event_t rx = {
                .type = BT_RX_EVENT_DATA,
                .len = (uint16_t)((param->data_ind.len - offset) > BT_PORT_RX_CHUNK_BYTES
                                      ? BT_PORT_RX_CHUNK_BYTES
                                      : (param->data_ind.len - offset)),
            };
            memcpy(rx.data, param->data_ind.data + offset, rx.len);
            if (xQueueSend(s_rx_queue, &rx, 0) != pdPASS) {
                uint32_t dropped;
                portENTER_CRITICAL(&s_state_mux);
                dropped = ++s_rx_dropped;
                portEXIT_CRITICAL(&s_state_mux);
                ESP_LOGE(TAG, "rfcomm_rx=overflow dropped_chunks=%" PRIu32
                              " action=disconnect_session", dropped);
                (void)bt_port_disconnect();
                break;
            }
            offset += rx.len;
        }
        break;
    }
    case ESP_SPP_CONG_EVT:
        if (param->cong.cong) {
            ESP_LOGW(TAG, "rfcomm_tx=congested");
        }
        break;
    case ESP_SPP_WRITE_EVT:
        if (param->write.status != ESP_SPP_SUCCESS) {
            ESP_LOGW(TAG, "rfcomm_tx=failed status=%d", param->write.status);
        }
        break;
    default:
        ESP_LOGD(TAG, "spp_event=%d", event);
        break;
    }
}

esp_err_t bt_port_start(bt_port_rx_cb_t rx_cb, bt_port_state_cb_t state_cb, void *context)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (rx_cb == NULL || state_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    s_rx_cb = rx_cb;
    s_state_cb = state_cb;
    s_callback_context = context;
    if (s_discovery_retry_timer == NULL) {
        const esp_timer_create_args_t retry_timer_args = {
            .callback = discovery_retry_callback,
            .arg = NULL,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "bt_disc_retry",
            .skip_unhandled_events = true,
        };
        esp_err_t timer_err = esp_timer_create(&retry_timer_args, &s_discovery_retry_timer);
        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG, "discovery=retry_timer_create_failed error=%s",
                     esp_err_to_name(timer_err));
            return timer_err;
        }
    }
    s_rx_queue = xQueueCreate(BT_PORT_RX_QUEUE_DEPTH, sizeof(bt_rx_event_t));
    if (s_rx_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(rx_dispatch_task, "bt_rx_dispatch", BT_PORT_RX_TASK_STACK_BYTES,
                    NULL, 5, NULL) != pdPASS) {
        vQueueDelete(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

#if defined(CONFIG_BT_BLE_ENABLED) && CONFIG_BT_BLE_ENABLED
    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt_controller_ble_mem_release=failed error=%s", esp_err_to_name(err));
        return err;
    }
#else
    esp_err_t err;
#endif
    esp_bt_controller_config_t controller_config = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    err = esp_bt_controller_init(&controller_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_init=failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bt_controller_enable=failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_init=failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bluedroid_enable=failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_gap_register_callback(gap_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gap_callback=register_failed error=%s", esp_err_to_name(err));
        return err;
    }

    err = esp_bt_gap_set_device_name(BT_PORT_DEVICE_NAME);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "device_name=set_failed error=%s", esp_err_to_name(err));
    }
    esp_bt_pin_code_t debug_pin = {0};
    memcpy(debug_pin, BT_PORT_DEBUG_PIN, sizeof(BT_PORT_DEBUG_PIN) - 1);
    err = esp_bt_gap_set_pin(ESP_BT_PIN_TYPE_FIXED,
                             sizeof(BT_PORT_DEBUG_PIN) - 1, debug_pin);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pairing_pin=set_failed error=%s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "pairing_pin=configured value=11111111");
    }

    esp_bt_io_cap_t io_capability = ESP_BT_IO_CAP_NONE;
    err = esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &io_capability, sizeof(io_capability));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ssp_io_capability=set_failed error=%s", esp_err_to_name(err));
    }

    err = esp_sdp_register_callback(sdp_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdp_callback=register_failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_sdp_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdp_init=failed error=%s", esp_err_to_name(err));
        return err;
    }
    err = esp_spp_register_callback(spp_callback);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spp_callback=register_failed error=%s", esp_err_to_name(err));
        return err;
    }
    esp_spp_cfg_t spp_config = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 2048,
    };
    err = esp_spp_enhanced_init(&spp_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spp_init=failed error=%s", esp_err_to_name(err));
        return err;
    }

    s_started = true;
    ESP_LOGI(TAG, "br_edr=starting role=initiator service_uuid=00000000-deca-fade-deca-deafdecacafe");
    if (!s_discovery_enabled) {
        ESP_LOGI(TAG, "discovery=waiting reason=browser_not_ready");
    }
    return ESP_OK;
}

void bt_port_set_discovery_enabled(bool enabled)
{
    if (s_discovery_enabled == enabled) return;
    s_discovery_enabled = enabled;
    ESP_LOGI(TAG, "discovery_gate=%s reason=%s", enabled ? "open" : "closed",
             enabled ? "browser_media_ready" : "browser_not_ready");
    if (enabled) {
        begin_peer_discovery();
    } else {
        cancel_discovery_retry();
        if (s_discovery_started) {
            esp_err_t err = esp_bt_gap_cancel_discovery();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "discovery=cancel_failed error=%s", esp_err_to_name(err));
            }
        }
    }
}

esp_err_t bt_port_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t handle;
    bool connected;
    portENTER_CRITICAL(&s_state_mux);
    connected = s_connected;
    handle = s_spp_handle;
    portEXIT_CRITICAL(&s_state_mux);
    if (!connected) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t offset = 0;
    while (offset < len) {
        size_t chunk_len = len - offset;
        if (chunk_len > BT_PORT_RX_CHUNK_BYTES) {
            chunk_len = BT_PORT_RX_CHUNK_BYTES;
        }
        esp_err_t err = esp_spp_write(handle, (int)chunk_len, (uint8_t *)data + offset);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "rfcomm_tx=enqueue_failed error=%s offset=%u", esp_err_to_name(err),
                     (unsigned)offset);
            return err;
        }
        offset += chunk_len;
    }
    return ESP_OK;
}

bool bt_port_is_connected(void)
{
    bool connected;
    portENTER_CRITICAL(&s_state_mux);
    connected = s_connected;
    portEXIT_CRITICAL(&s_state_mux);
    return connected;
}

esp_err_t bt_port_disconnect(void)
{
    uint32_t handle;
    bool connected;
    portENTER_CRITICAL(&s_state_mux);
    connected = s_connected;
    handle = s_spp_handle;
    portEXIT_CRITICAL(&s_state_mux);
    if (!connected) return ESP_ERR_INVALID_STATE;

    if (s_rx_queue != NULL) (void)xQueueReset(s_rx_queue);
    esp_err_t err = esp_spp_disconnect(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rfcomm=disconnect_failed reason=receive_stream_unreliable error=%s",
                 esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "rfcomm=disconnecting reason=receive_stream_unreliable");
    }
    /* Reset the protocol consumer immediately; SPP_CLOSE may arrive later. */
    set_connected(false, 0);
    return err;
}

uint32_t bt_port_rx_dropped_chunks(void)
{
    uint32_t dropped;
    portENTER_CRITICAL(&s_state_mux);
    dropped = s_rx_dropped;
    portEXIT_CRITICAL(&s_state_mux);
    return dropped;
}
