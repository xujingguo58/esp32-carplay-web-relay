#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NO_MEM 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
static inline const char *esp_err_to_name(esp_err_t error)
{
    (void)error;
    return "host_stub";
}

#define ESP_LOGD(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); } while (0)

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(mux) do { (void)(mux); } while (0)
#define portEXIT_CRITICAL(mux) do { (void)(mux); } while (0)

typedef int BaseType_t;
typedef uint32_t UBaseType_t;
typedef struct mock_queue {
    size_t depth;
    size_t item_size;
    size_t count;
    size_t head;
    uint8_t *items;
} *QueueHandle_t;
typedef void *TaskHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
static inline QueueHandle_t xQueueCreate(UBaseType_t depth, UBaseType_t item_size)
{
    QueueHandle_t queue = calloc(1, sizeof(*queue));
    if (queue == NULL) return NULL;
    queue->depth = depth;
    queue->item_size = item_size;
    queue->items = calloc(depth, item_size);
    if (queue->items == NULL) {
        free(queue);
        return NULL;
    }
    return queue;
}
static inline BaseType_t xQueueSend(QueueHandle_t queue, const void *item, uint32_t ticks)
{
    (void)ticks;
    if (queue == NULL || item == NULL || queue->count == queue->depth) return pdFALSE;
    size_t tail = (queue->head + queue->count) % queue->depth;
    memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
    ++queue->count;
    return pdPASS;
}
static inline BaseType_t xQueueReceive(QueueHandle_t queue, void *item, uint32_t ticks)
{
    (void)ticks;
    if (queue == NULL || item == NULL || queue->count == 0) return pdFALSE;
    memcpy(item, queue->items + queue->head * queue->item_size, queue->item_size);
    queue->head = (queue->head + 1U) % queue->depth;
    --queue->count;
    return pdTRUE;
}
static inline BaseType_t xQueueReset(QueueHandle_t queue)
{
    if (queue == NULL) return pdFALSE;
    queue->count = 0;
    queue->head = 0;
    return pdPASS;
}
static inline void vQueueDelete(QueueHandle_t queue)
{
    if (queue == NULL) return;
    free(queue->items);
    free(queue);
}
static inline BaseType_t xTaskCreate(void (*entry)(void *), const char *name,
                                     uint32_t stack, void *argument,
                                     UBaseType_t priority, TaskHandle_t *task)
{
    (void)entry; (void)name; (void)stack; (void)argument; (void)priority; (void)task;
    return pdPASS;
}

typedef enum { ESP_BT_MODE_BLE, ESP_BT_MODE_CLASSIC_BT } esp_bt_mode_t;
typedef struct { int unused; } esp_bt_controller_config_t;
#define BT_CONTROLLER_INIT_CONFIG_DEFAULT() ((esp_bt_controller_config_t){0})
static inline esp_err_t esp_bt_controller_mem_release(esp_bt_mode_t mode)
{ (void)mode; return ESP_OK; }
static inline esp_err_t esp_bt_controller_init(const esp_bt_controller_config_t *config)
{ (void)config; return ESP_OK; }
static inline esp_err_t esp_bt_controller_enable(esp_bt_mode_t mode)
{ (void)mode; return ESP_OK; }
static inline esp_err_t esp_bluedroid_init(void) { return ESP_OK; }
static inline esp_err_t esp_bluedroid_enable(void) { return ESP_OK; }

typedef uint8_t esp_bt_pin_code_t[16];
typedef enum { ESP_BT_IO_CAP_NONE } esp_bt_io_cap_t;
typedef enum { ESP_BT_CONNECTABLE, ESP_BT_NON_CONNECTABLE } esp_bt_connection_mode_t;
typedef enum { ESP_BT_GENERAL_DISCOVERABLE, ESP_BT_LIMITED_DISCOVERABLE,
               ESP_BT_NON_DISCOVERABLE } esp_bt_discovery_mode_t;
typedef enum { ESP_BT_GAP_AUTH_CMPL_EVT, ESP_BT_GAP_CFM_REQ_EVT,
               ESP_BT_GAP_PIN_REQ_EVT, ESP_BT_GAP_MODE_CHG_EVT } esp_bt_gap_cb_event_t;
typedef struct {
    union {
        struct { int stat; uint8_t device_name[64]; } auth_cmpl;
        struct { uint8_t bda[6]; } cfm_req;
        struct { uint8_t bda[6]; } pin_req;
        struct { int mode; } mode_chg;
    };
} esp_bt_gap_cb_param_t;
typedef void (*esp_bt_gap_cb_t)(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *);
static inline esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t callback)
{ (void)callback; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_security_param(int parameter, void *value, uint8_t length)
{ (void)parameter; (void)value; (void)length; return ESP_OK; }
static inline esp_err_t esp_bt_gap_ssp_confirm_reply(const uint8_t address[6], bool accept)
{ (void)address; (void)accept; return ESP_OK; }
static inline esp_err_t esp_bt_gap_pin_reply(const uint8_t address[6], bool accept,
                                             uint8_t length, esp_bt_pin_code_t pin)
{ (void)address; (void)accept; (void)length; (void)pin; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_device_name(const char *name)
{ (void)name; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t connection,
                                                 esp_bt_discovery_mode_t discovery)
{ (void)connection; (void)discovery; return ESP_OK; }
#define ESP_BT_SP_IOCAP_MODE 0

#define ESP_UUID_LEN_128 16
typedef struct { uint8_t uuid128[16]; } mock_sdp_uuid_value_t;
typedef struct { uint8_t len; mock_sdp_uuid_value_t uuid; } mock_sdp_uuid_t;
typedef struct {
    int type;
    mock_sdp_uuid_t uuid;
    uint32_t service_name_length;
    char *service_name;
    int rfcomm_channel_number;
    int l2cap_psm;
    int profile_version;
} mock_sdp_header_t;
typedef struct { mock_sdp_header_t hdr; } esp_bluetooth_sdp_raw_record_t;
typedef esp_bluetooth_sdp_raw_record_t esp_bluetooth_sdp_record_t;
#define ESP_SDP_TYPE_RAW 0
typedef enum { ESP_SDP_INIT_EVT, ESP_SDP_CREATE_RECORD_COMP_EVT,
               ESP_SDP_DEINIT_EVT } esp_sdp_cb_event_t;
#define ESP_SDP_SUCCESS 0
typedef struct {
    union {
        struct { int status; } init;
        struct { int status; int record_handle; } create_record;
        struct { int status; } deinit;
    };
} esp_sdp_cb_param_t;
typedef void (*esp_sdp_cb_t)(esp_sdp_cb_event_t, esp_sdp_cb_param_t *);
static inline esp_err_t esp_sdp_register_callback(esp_sdp_cb_t callback)
{ (void)callback; return ESP_OK; }
static inline esp_err_t esp_sdp_init(void) { return ESP_OK; }
static inline esp_err_t esp_sdp_create_record(esp_bluetooth_sdp_record_t *record)
{ (void)record; return ESP_OK; }

typedef enum { ESP_SPP_INIT_EVT, ESP_SPP_START_EVT, ESP_SPP_SRV_OPEN_EVT,
               ESP_SPP_CLOSE_EVT, ESP_SPP_DATA_IND_EVT, ESP_SPP_CONG_EVT,
               ESP_SPP_WRITE_EVT } esp_spp_cb_event_t;
#define ESP_SPP_SUCCESS 0
#define ESP_SPP_SEC_AUTHENTICATE 0x01
#define ESP_SPP_SEC_ENCRYPT 0x02
typedef enum { ESP_SPP_ROLE_SLAVE } esp_spp_role_t;
typedef enum { ESP_SPP_MODE_CB } esp_spp_mode_t;
typedef struct {
    union {
        struct { int status; } init;
        struct { int status; uint8_t scn; } start;
        struct { int status; uint32_t handle; } srv_open;
        struct { int status; uint32_t handle; } close;
        struct { int status; uint32_t handle; uint8_t *data; uint16_t len; } data_ind;
        struct { bool cong; } cong;
        struct { int status; uint32_t handle; } write;
    };
} esp_spp_cb_param_t;
typedef void (*esp_spp_cb_t)(esp_spp_cb_event_t, esp_spp_cb_param_t *);
typedef struct {
    int local_scn;
    bool create_spp_record;
    uint8_t sec_mask;
    esp_spp_role_t role;
    const char *name;
} esp_spp_start_srv_cfg_t;
typedef struct {
    esp_spp_mode_t mode;
    bool enable_l2cap_ertm;
    uint16_t tx_buffer_size;
} esp_spp_cfg_t;
static inline esp_err_t esp_spp_register_callback(esp_spp_cb_t callback)
{ (void)callback; return ESP_OK; }
static inline esp_err_t esp_spp_start_srv_with_cfg(const esp_spp_start_srv_cfg_t *config)
{ (void)config; return ESP_OK; }
static inline esp_err_t esp_spp_enhanced_init(const esp_spp_cfg_t *config)
{ (void)config; return ESP_OK; }
static inline esp_err_t esp_spp_write(uint32_t handle, int length, uint8_t *data)
{ (void)handle; (void)length; (void)data; return ESP_OK; }
static unsigned mock_spp_disconnect_call_count;
static inline esp_err_t esp_spp_disconnect(uint32_t handle)
{ (void)handle; ++mock_spp_disconnect_call_count; return ESP_OK; }
