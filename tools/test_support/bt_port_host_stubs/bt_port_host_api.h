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
typedef uint8_t esp_bd_addr_t[6];
#define ESP_BD_ADDR_LEN 6U
typedef uint8_t esp_bt_status_t;
typedef uint8_t esp_bt_link_key_type_t;
typedef struct {
    uint16_t len;
    union {
        uint16_t uuid16;
        uint32_t uuid32;
        uint8_t uuid128[16];
    } uuid;
} esp_bt_uuid_t;
typedef enum { ESP_BT_IO_CAP_NONE } esp_bt_io_cap_t;
typedef enum { ESP_BT_NON_CONNECTABLE, ESP_BT_CONNECTABLE } esp_bt_connection_mode_t;
typedef enum { ESP_BT_NON_DISCOVERABLE, ESP_BT_LIMITED_DISCOVERABLE,
               ESP_BT_GENERAL_DISCOVERABLE } esp_bt_discovery_mode_t;
typedef enum { ESP_BT_PIN_TYPE_VARIABLE, ESP_BT_PIN_TYPE_FIXED } esp_bt_pin_type_t;
typedef enum { ESP_BT_GAP_DISCOVERY_STOPPED, ESP_BT_GAP_DISCOVERY_STARTED }
    esp_bt_gap_discovery_state_t;
typedef enum { ESP_BT_GAP_DEV_PROP_BDNAME = 1, ESP_BT_GAP_DEV_PROP_COD,
               ESP_BT_GAP_DEV_PROP_RSSI, ESP_BT_GAP_DEV_PROP_EIR }
    esp_bt_gap_dev_prop_type_t;
typedef struct {
    esp_bt_gap_dev_prop_type_t type;
    int len;
    void *val;
} esp_bt_gap_dev_prop_t;
typedef enum { ESP_BT_GAP_DISC_RES_EVT, ESP_BT_GAP_DISC_STATE_CHANGED_EVT,
               ESP_BT_GAP_AUTH_CMPL_EVT, ESP_BT_GAP_PIN_REQ_EVT,
               ESP_BT_GAP_CFM_REQ_EVT, ESP_BT_GAP_KEY_NOTIF_EVT,
               ESP_BT_GAP_KEY_REQ_EVT, ESP_BT_GAP_MODE_CHG_EVT,
               ESP_BT_GAP_CONFIG_EIR_DATA_EVT }
    esp_bt_gap_cb_event_t;
typedef struct {
    union {
        struct { esp_bd_addr_t bda; int num_prop; esp_bt_gap_dev_prop_t *prop; } disc_res;
        struct { esp_bt_gap_discovery_state_t state; } disc_st_chg;
        struct { esp_bd_addr_t bda; int stat; uint8_t device_name[249]; } auth_cmpl;
        struct { esp_bd_addr_t bda; bool min_16_digit; } pin_req;
        struct { esp_bd_addr_t bda; uint32_t num_val; } cfm_req;
        struct { esp_bd_addr_t bda; uint32_t passkey; } key_notif;
        struct { esp_bd_addr_t bda; } key_req;
        struct { int mode; } mode_chg;
        struct { int stat; uint8_t eir_type_num; int eir_type[8]; } config_eir_data;
    };
} esp_bt_gap_cb_param_t;
typedef void (*esp_bt_gap_cb_t)(esp_bt_gap_cb_event_t, esp_bt_gap_cb_param_t *);
static inline esp_err_t esp_bt_gap_register_callback(esp_bt_gap_cb_t callback)
{ (void)callback; return ESP_OK; }
typedef enum { ESP_BT_INQ_MODE_GENERAL_INQUIRY, ESP_BT_INQ_MODE_LIMITED_INQUIRY }
    esp_bt_inq_mode_t;
#define ESP_BT_COD_MAJOR_DEV_PHONE 2U
#define ESP_BT_COD_MAJOR_DEV_AV 4U
#define ESP_BT_COD_SRVC_RENDERING 0x20U
#define ESP_BT_COD_SRVC_AUDIO 0x100U
#define ESP_BT_COD_SRVC_TELEPHONY 0x200U
#define ESP_BT_EIR_FLAG_GEN_DISC 0x02U
#define ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME 0x09U
#define ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME 0x08U
#define ESP_BT_SP_IOCAP_MODE 0
typedef struct {
    uint32_t reserved_2 : 2;
    uint32_t minor : 6;
    uint32_t major : 5;
    uint32_t service : 11;
    uint32_t reserved_8 : 8;
} esp_bt_cod_t;
typedef enum { ESP_BT_SET_COD_MAJOR_MINOR, ESP_BT_SET_COD_SERVICE_CLASS,
               ESP_BT_CLR_COD_SERVICE_CLASS, ESP_BT_SET_COD_ALL } esp_bt_cod_mode_t;
typedef struct {
    bool fec_required;
    bool include_txpower;
    bool include_uuid;
    bool include_name;
    uint8_t flag;
    uint16_t manufacturer_len;
    uint8_t *p_manufacturer_data;
    uint16_t url_len;
    uint8_t *p_url;
} esp_bt_eir_data_t;
static unsigned mock_scan_mode_call_count;
static esp_bt_connection_mode_t mock_connection_mode;
static esp_bt_discovery_mode_t mock_discovery_mode;
static unsigned mock_cod_call_count;
static esp_bt_cod_t mock_cod;
static esp_bt_cod_mode_t mock_cod_mode;
static unsigned mock_eir_call_count;
static esp_bt_eir_data_t mock_eir_data;
static inline esp_err_t esp_bt_gap_start_discovery(esp_bt_inq_mode_t mode,
                                                   uint8_t inq_len,
                                                   uint8_t num_responses)
{ (void)mode; (void)inq_len; (void)num_responses; return ESP_OK; }
static inline esp_err_t esp_bt_gap_cancel_discovery(void) { return ESP_OK; }
static inline uint8_t *esp_bt_gap_resolve_eir_data(uint8_t *eir, uint8_t type,
                                                   uint8_t *length)
{ (void)eir; (void)type; if (length != NULL) *length = 0U; return NULL; }
static inline esp_err_t esp_bt_gap_set_security_param(int parameter, void *value, uint8_t length)
{ (void)parameter; (void)value; (void)length; return ESP_OK; }
static inline esp_err_t esp_bt_gap_ssp_confirm_reply(const uint8_t address[6], bool accept)
{ (void)address; (void)accept; return ESP_OK; }
static inline esp_err_t esp_bt_gap_ssp_passkey_reply(const uint8_t address[6],
                                                     bool accept, uint32_t passkey)
{ (void)address; (void)accept; (void)passkey; return ESP_OK; }
static inline esp_err_t esp_bt_gap_pin_reply(const uint8_t address[6], bool accept,
                                             uint8_t length, esp_bt_pin_code_t pin)
{ (void)address; (void)accept; (void)length; (void)pin; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_pin(esp_bt_pin_type_t type, uint8_t length,
                                           esp_bt_pin_code_t pin)
{ (void)type; (void)length; (void)pin; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_device_name(const char *name)
{ (void)name; return ESP_OK; }
static inline esp_err_t esp_bt_gap_set_scan_mode(esp_bt_connection_mode_t connection,
                                                 esp_bt_discovery_mode_t discovery)
{
    ++mock_scan_mode_call_count;
    mock_connection_mode = connection;
    mock_discovery_mode = discovery;
    return ESP_OK;
}
static inline esp_err_t esp_bt_gap_set_cod(esp_bt_cod_t cod, esp_bt_cod_mode_t mode)
{
    ++mock_cod_call_count;
    mock_cod = cod;
    mock_cod_mode = mode;
    return ESP_OK;
}
static inline esp_err_t esp_bt_gap_config_eir_data(esp_bt_eir_data_t *eir_data)
{
    ++mock_eir_call_count;
    if (eir_data != NULL) mock_eir_data = *eir_data;
    return ESP_OK;
}

#define ESP_UUID_LEN_128 16
typedef struct {
    int type;
    esp_bt_uuid_t uuid;
    uint32_t service_name_length;
    char *service_name;
    int32_t rfcomm_channel_number;
    int32_t l2cap_psm;
    int32_t profile_version;
    int user1_ptr_len;
    uint8_t *user1_ptr;
} mock_sdp_header_t;
typedef struct {
    mock_sdp_header_t hdr;
} esp_bluetooth_sdp_raw_record_t;
typedef esp_bluetooth_sdp_raw_record_t esp_bluetooth_sdp_record_t;
#define ESP_SDP_TYPE_RAW 0
typedef enum { ESP_SDP_INIT_EVT, ESP_SDP_DEINIT_EVT, ESP_SDP_SEARCH_COMP_EVT,
               ESP_SDP_CREATE_RECORD_COMP_EVT } esp_sdp_cb_event_t;
#define ESP_SDP_SUCCESS 0
typedef struct {
    union {
        struct { int status; } init;
        struct { int status; esp_bd_addr_t remote_addr; esp_bt_uuid_t sdp_uuid;
                 int record_count; esp_bluetooth_sdp_record_t *records; } search;
        struct { int status; int record_handle; } create_record;
        struct { int status; } deinit;
    };
} esp_sdp_cb_param_t;
typedef void (*esp_sdp_cb_t)(esp_sdp_cb_event_t, esp_sdp_cb_param_t *);
static inline esp_err_t esp_sdp_register_callback(esp_sdp_cb_t callback)
{ (void)callback; return ESP_OK; }
static inline esp_err_t esp_sdp_init(void) { return ESP_OK; }
static inline esp_err_t esp_sdp_search_record(esp_bd_addr_t address, esp_bt_uuid_t uuid)
{ (void)address; (void)uuid; return ESP_OK; }
static inline esp_err_t esp_sdp_create_record(esp_bluetooth_sdp_record_t *record)
{ (void)record; return ESP_OK; }

typedef enum { ESP_SPP_INIT_EVT, ESP_SPP_START_EVT, ESP_SPP_OPEN_EVT,
               ESP_SPP_CLOSE_EVT, ESP_SPP_DATA_IND_EVT, ESP_SPP_CONG_EVT,
               ESP_SPP_WRITE_EVT, ESP_SPP_SRV_OPEN_EVT, ESP_SPP_CL_INIT_EVT }
    esp_spp_cb_event_t;
#define ESP_SPP_SUCCESS 0
typedef uint8_t esp_spp_sec_t;
#define ESP_SPP_SEC_AUTHENTICATE 0x12
#define ESP_SPP_SEC_ENCRYPT 0x24
typedef enum { ESP_SPP_ROLE_MASTER, ESP_SPP_ROLE_SLAVE } esp_spp_role_t;
typedef enum { ESP_SPP_MODE_CB } esp_spp_mode_t;
typedef struct {
    union {
        struct { int status; } init;
        struct { int status; uint8_t scn; } start;
        struct { int status; uint32_t handle; int fd; esp_bd_addr_t rem_bda; } open;
        struct { int status; } cl_init;
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
static inline esp_err_t esp_spp_connect(esp_spp_sec_t security, esp_spp_role_t role,
                                        uint8_t channel, esp_bd_addr_t address)
{ (void)security; (void)role; (void)channel; (void)address; return ESP_OK; }
static inline esp_err_t esp_spp_write(uint32_t handle, int length, uint8_t *data)
{ (void)handle; (void)length; (void)data; return ESP_OK; }
static unsigned mock_spp_disconnect_call_count;
static inline esp_err_t esp_spp_disconnect(uint32_t handle)
{ (void)handle; ++mock_spp_disconnect_call_count; return ESP_OK; }

typedef void *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *);
typedef enum { ESP_TIMER_TASK } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;
static inline esp_err_t esp_timer_create(const esp_timer_create_args_t *args,
                                         esp_timer_handle_t *timer)
{ (void)args; if (timer != NULL) *timer = (void *)1; return ESP_OK; }
static inline esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us)
{ (void)timer; (void)timeout_us; return ESP_OK; }
static inline esp_err_t esp_timer_stop(esp_timer_handle_t timer)
{ (void)timer; return ESP_OK; }
