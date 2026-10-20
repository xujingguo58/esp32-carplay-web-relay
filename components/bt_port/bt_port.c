#include "bt_port.h"

#include <inttypes.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_sdp_api.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define BT_PORT_DEVICE_NAME "ESP32-CarPlay"
#define BT_PORT_IAP2_UUID_BYTES { \
    0x00, 0x00, 0x00, 0x00, 0xde, 0xca, 0xfa, 0xde, \
    0xde, 0xca, 0xde, 0xaf, 0xde, 0xca, 0xca, 0xfe \
}
#define BT_PORT_RFCOMM_CHANNEL 3
#define BT_PORT_RX_CHUNK_BYTES 512
#define BT_PORT_RX_QUEUE_DEPTH 8
#define BT_PORT_RX_TASK_STACK_BYTES 4096

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
static bool s_spp_server_ready;
static bool s_sdp_record_created;

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
    case ESP_BT_GAP_AUTH_CMPL_EVT:
        ESP_LOGI(TAG, "pairing_complete status=%d name=%s", param->auth_cmpl.stat,
                 (const char *)param->auth_cmpl.device_name);
        break;
    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "pairing_confirmation=just_works_accept");
        (void)esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin_code = {0};
        ESP_LOGW(TAG, "pairing_request=legacy_pin_rejected; Secure Simple Pairing is required");
        (void)esp_bt_gap_pin_reply(param->pin_req.bda, false, 0, pin_code);
        break;
    }
    case ESP_BT_GAP_MODE_CHG_EVT:
        ESP_LOGI(TAG, "radio_mode=changed mode=%d", param->mode_chg.mode);
        break;
    default:
        ESP_LOGD(TAG, "gap_event=%d", event);
        break;
    }
}

static void create_iap2_sdp_record(void)
{
    if (s_sdp_record_created) {
        return;
    }
    static const uint8_t uuid[ESP_UUID_LEN_128] = BT_PORT_IAP2_UUID_BYTES;
    static char service_name[] = "iAP2 Accessory";
    esp_bluetooth_sdp_raw_record_t record = {0};
    record.hdr.type = ESP_SDP_TYPE_RAW;
    record.hdr.uuid.len = ESP_UUID_LEN_128;
    memcpy(record.hdr.uuid.uuid.uuid128, uuid, sizeof(uuid));
    record.hdr.service_name_length = sizeof(service_name);
    record.hdr.service_name = service_name;
    record.hdr.rfcomm_channel_number = BT_PORT_RFCOMM_CHANNEL;
    record.hdr.l2cap_psm = -1;
    record.hdr.profile_version = 0x0100;
    esp_err_t err = esp_sdp_create_record((esp_bluetooth_sdp_record_t *)&record);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sdp_record=create_failed error=%s", esp_err_to_name(err));
        return;
    }
    s_sdp_record_created = true;
    ESP_LOGI(TAG, "sdp_record=registering service=iAP2 rfcomm_channel=%d",
             BT_PORT_RFCOMM_CHANNEL);
}

static void sdp_callback(esp_sdp_cb_event_t event, esp_sdp_cb_param_t *param)
{
    switch (event) {
    case ESP_SDP_INIT_EVT:
        if (param->init.status == ESP_SDP_SUCCESS) {
            s_sdp_ready = true;
            create_iap2_sdp_record();
        } else {
            ESP_LOGE(TAG, "sdp_init=failed status=%d", param->init.status);
        }
        break;
    case ESP_SDP_CREATE_RECORD_COMP_EVT:
        if (param->create_record.status == ESP_SDP_SUCCESS) {
            ESP_LOGI(TAG, "sdp_record=ready handle=%d", param->create_record.record_handle);
            (void)esp_bt_gap_set_device_name(BT_PORT_DEVICE_NAME);
            (void)esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
        } else {
            ESP_LOGE(TAG, "sdp_record=create_failed status=%d", param->create_record.status);
        }
        break;
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
        ESP_LOGI(TAG, "spp_init=ready mode=callback");
        if (s_sdp_ready) {
            create_iap2_sdp_record();
        }
        esp_spp_start_srv_cfg_t server = {
            .local_scn = BT_PORT_RFCOMM_CHANNEL,
            .create_spp_record = false,
            .sec_mask = ESP_SPP_SEC_AUTHENTICATE | ESP_SPP_SEC_ENCRYPT,
            .role = ESP_SPP_ROLE_SLAVE,
            .name = "iAP2 Accessory",
        };
        esp_err_t err = esp_spp_start_srv_with_cfg(&server);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "spp_server=start_failed error=%s", esp_err_to_name(err));
        }
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            s_spp_server_ready = true;
            ESP_LOGI(TAG, "spp_server=ready scn=%u", param->start.scn);
        } else {
            ESP_LOGE(TAG, "spp_server=start_failed status=%d", param->start.status);
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        if (param->srv_open.status == ESP_SPP_SUCCESS) {
            ESP_LOGI(TAG, "rfcomm=connected handle=%" PRIu32,
                     param->srv_open.handle);
            set_connected(true, param->srv_open.handle);
        } else {
            ESP_LOGW(TAG, "rfcomm=connect_failed status=%d", param->srv_open.status);
        }
        break;
    case ESP_SPP_CLOSE_EVT:
        ESP_LOGI(TAG, "rfcomm=closed status=%d", param->close.status);
        if (s_rx_queue != NULL) {
            (void)xQueueReset(s_rx_queue);
        }
        set_connected(false, 0);
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

    esp_err_t err = esp_bt_controller_mem_release(ESP_BT_MODE_BLE);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "bt_controller_ble_mem_release=failed error=%s", esp_err_to_name(err));
        return err;
    }
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
    ESP_LOGI(TAG, "br_edr=starting service_uuid=00000000-deca-fade-deca-deafdecacafe");
    return ESP_OK;
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
