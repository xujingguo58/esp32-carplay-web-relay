#include "media_bridge.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_peer_default.h"
#include "media_frame_gate.h"
#include "media_keyframe_gate.h"
#include "media_timestamp.h"
#include "time_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/platform_util.h"

#ifndef CONFIG_CARPLAY_WEBRTC_TEST_STREAM
#define CONFIG_CARPLAY_WEBRTC_TEST_STREAM 0
#endif

#define MEDIA_BRIDGE_TASK_STACK_BYTES (16 * 1024)
#define MEDIA_BRIDGE_TASK_PRIORITY    (5)
#define MEDIA_BRIDGE_LOOP_DELAY_MS    (10)
#define MEDIA_BRIDGE_FRAME_PERIOD_US  (100000)
#define MEDIA_BRIDGE_SDP_MAX_BYTES    (3072)
#define MEDIA_BRIDGE_RTP_SEND_POOL    (16384)
#define MEDIA_BRIDGE_RTP_SEND_QUEUE   (16)
#define MEDIA_BRIDGE_MAX_LIVE_FRAME_BYTES (8U * 1024U * 1024U + 4096U)
#define MEDIA_BRIDGE_LIVE_CONFIG_BYTES 4096U

static const char *TAG = "media_bridge";

typedef struct {
    char *sdp;
    size_t size;
} answer_command_t;

static void release_answer_command(answer_command_t *answer)
{
    if (answer == NULL || answer->sdp == NULL) return;
    mbedtls_platform_zeroize(answer->sdp, answer->size + 1U);
    heap_caps_free(answer->sdp);
    answer->sdp = NULL;
    answer->size = 0U;
}

typedef struct {
    size_t size;
    uint32_t pts;
    uint32_t keyframe_generation;
    bool is_keyframe;
} live_frame_command_t;

static esp_peer_handle_t s_peer;
static TaskHandle_t s_task;
static QueueHandle_t s_answer_queue;
static QueueHandle_t s_live_frame_queue;
static EventGroupHandle_t s_task_events;
static media_bridge_signal_cb_t s_signal_cb;
static void *s_signal_context;
static volatile bool s_stop_requested;
static volatile bool s_started;
static volatile bool s_connected;
static media_keyframe_gate_t s_keyframe_gate;
static volatile uint32_t s_frames_sent;
static volatile uint32_t s_frames_dropped;
static volatile esp_peer_state_t s_peer_state = ESP_PEER_STATE_CLOSED;
static portMUX_TYPE s_live_mux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *s_live_buffer;
static size_t s_live_buffer_capacity;
static uint32_t s_live_buffer_caps;
static uint8_t s_live_config[MEDIA_BRIDGE_LIVE_CONFIG_BYTES];
static size_t s_live_config_len;
static bool s_live_buffer_busy;
static media_frame_gate_t s_live_frame_gate;
static volatile bool s_live_source;
static media_timestamp_mapper_t s_live_timestamp_mapper;
static bool s_live_pts_source_known;
static bool s_live_pts_source_is_sender;

#define MEDIA_BRIDGE_TASK_STOPPED_BIT BIT0

static void reset_keyframe_gate(void)
{
    portENTER_CRITICAL(&s_live_mux);
    media_keyframe_gate_reset(&s_keyframe_gate);
    portEXIT_CRITICAL(&s_live_mux);
}

static bool keyframe_gate_accepts(bool is_keyframe)
{
    portENTER_CRITICAL(&s_live_mux);
    bool accepts = media_keyframe_gate_accepts(&s_keyframe_gate, is_keyframe);
    portEXIT_CRITICAL(&s_live_mux);
    return accepts;
}

static uint32_t keyframe_gate_generation(void)
{
    portENTER_CRITICAL(&s_live_mux);
    uint32_t generation = media_keyframe_gate_generation(&s_keyframe_gate);
    portEXIT_CRITICAL(&s_live_mux);
    return generation;
}

static void request_upstream_keyframe(void)
{
    if (!s_connected || s_signal_cb == NULL) return;
    esp_err_t result = s_signal_cb("keyframe-request", "main", s_signal_context);
    if (result != ESP_OK) {
        ESP_LOGD(TAG, "upstream_keyframe_request=unavailable error=%d", result);
    }
}

static void keyframe_gate_note_drop(bool is_keyframe)
{
    portENTER_CRITICAL(&s_live_mux);
    media_keyframe_gate_note_drop(&s_keyframe_gate, is_keyframe);
    portEXIT_CRITICAL(&s_live_mux);
    request_upstream_keyframe();
}

static void keyframe_gate_note_keyframe_queued(uint32_t generation)
{
    portENTER_CRITICAL(&s_live_mux);
    media_keyframe_gate_note_keyframe_queued(&s_keyframe_gate, generation);
    portEXIT_CRITICAL(&s_live_mux);
}

static void keyframe_gate_note_queue_failed(bool is_keyframe, uint32_t generation)
{
    portENTER_CRITICAL(&s_live_mux);
    media_keyframe_gate_note_queue_failed(&s_keyframe_gate, is_keyframe, generation);
    portEXIT_CRITICAL(&s_live_mux);
    request_upstream_keyframe();
}

static void keyframe_gate_note_send(bool is_keyframe,
                                    bool sent,
                                    uint32_t generation)
{
    portENTER_CRITICAL(&s_live_mux);
    media_keyframe_gate_note_send(&s_keyframe_gate, is_keyframe, sent, generation);
    portEXIT_CRITICAL(&s_live_mux);
    if (!sent) request_upstream_keyframe();
}

#if CONFIG_CARPLAY_WEBRTC_TEST_STREAM
extern const uint8_t _binary_test_stream_bin_start[];
extern const uint8_t _binary_test_stream_bin_end[];
static size_t s_stream_offset;
static int64_t s_next_frame_time_us;
static uint32_t s_video_pts;
#endif

static bool frame_has_idr(const uint8_t *data, size_t size)
{
    if (data == NULL) return false;
    for (size_t index = 0; index + 4 < size; index++) {
        size_t header = 0;
        if (data[index] == 0 && data[index + 1] == 0 && data[index + 2] == 1) {
            header = index + 3;
        } else if (index + 5 < size && data[index] == 0 && data[index + 1] == 0 &&
                   data[index + 2] == 0 && data[index + 3] == 1) {
            header = index + 4;
        }
        if (header != 0 && (data[header] & 0x1f) == 5) return true;
    }
    return false;
}

static bool grow_live_buffer(size_t required)
{
    if (required <= s_live_buffer_capacity) return true;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    uint32_t caps = s_live_buffer_caps;
    void *buffer = NULL;

    if (s_live_buffer != NULL && caps == psram_caps) {
        buffer = heap_caps_realloc(s_live_buffer, required, psram_caps);
        caps = psram_caps;
    } else if (s_live_buffer == NULL && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0) {
        buffer = heap_caps_malloc(required, psram_caps);
        caps = psram_caps;
    }

    /* A previous small frame may have fallen back to internal RAM. Move it to
     * PSRAM before growing, so one allocation failure does not pin the stream
     * to the smaller internal heap for the rest of the session. */
    if (buffer == NULL && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) != 0) {
        void *psram_buffer = heap_caps_malloc(required, psram_caps);
        if (psram_buffer != NULL) {
            if (s_live_buffer != NULL) {
                memcpy(psram_buffer, s_live_buffer, s_live_buffer_capacity);
                heap_caps_free(s_live_buffer);
            }
            buffer = psram_buffer;
            caps = psram_caps;
        }
    }
    if (buffer == NULL && required <= 64U * 1024U) {
        if (s_live_buffer != NULL && s_live_buffer_caps == internal_caps) {
            buffer = heap_caps_realloc(s_live_buffer, required, internal_caps);
        } else {
            buffer = heap_caps_malloc(required, internal_caps);
            if (buffer != NULL && s_live_buffer != NULL) heap_caps_free(s_live_buffer);
        }
        caps = internal_caps;
    }
    if (buffer == NULL) return false;
    s_live_buffer = buffer;
    s_live_buffer_capacity = required;
    s_live_buffer_caps = caps;
    return true;
}

static QueueHandle_t begin_live_frame_submission(void)
{
    QueueHandle_t queue = NULL;
    portENTER_CRITICAL(&s_live_mux);
    if (s_live_frame_queue != NULL &&
        media_frame_gate_enter(&s_live_frame_gate)) {
        queue = s_live_frame_queue;
    }
    portEXIT_CRITICAL(&s_live_mux);
    return queue;
}

static void end_live_frame_submission(void)
{
    portENTER_CRITICAL(&s_live_mux);
    media_frame_gate_leave(&s_live_frame_gate);
    portEXIT_CRITICAL(&s_live_mux);
}

static void delete_media_queues(void)
{
    QueueHandle_t answer_queue = s_answer_queue;
    s_answer_queue = NULL;
    portENTER_CRITICAL(&s_live_mux);
    media_frame_gate_stop(&s_live_frame_gate);
    QueueHandle_t live_frame_queue = s_live_frame_queue;
    s_live_frame_queue = NULL;
    portEXIT_CRITICAL(&s_live_mux);
    if (answer_queue != NULL) {
        answer_command_t pending_answer;
        while (xQueueReceive(answer_queue, &pending_answer, 0) == pdTRUE) {
            release_answer_command(&pending_answer);
        }
        vQueueDelete(answer_queue);
    }
    if (live_frame_queue != NULL) vQueueDelete(live_frame_queue);
}

static void send_queued_live_frame(const live_frame_command_t *command)
{
    if (command == NULL) return;
    if (!s_connected || s_peer == NULL || command->size == 0) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(false);
        goto release_slot;
    }
    if (!keyframe_gate_accepts(command->is_keyframe)) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(command->is_keyframe);
        goto release_slot;
    }
    esp_peer_video_frame_t frame = {
        .pts = command->pts,
        .data = s_live_buffer,
        .size = (int)command->size,
    };
    int result = esp_peer_send_video(s_peer, &frame);
    if (result == ESP_PEER_ERR_NONE) {
        ++s_frames_sent;
        keyframe_gate_note_send(command->is_keyframe, true,
                                command->keyframe_generation);
    } else {
        ++s_frames_dropped;
        keyframe_gate_note_send(command->is_keyframe, false,
                                command->keyframe_generation);
        ESP_LOGW(TAG, "live_video_send=failed result=%d bytes=%u", result, (unsigned)command->size);
    }

release_slot:
    portENTER_CRITICAL(&s_live_mux);
    s_live_buffer_busy = false;
    portEXIT_CRITICAL(&s_live_mux);
}

static const char *peer_state_name(esp_peer_state_t state)
{
    switch (state) {
    case ESP_PEER_STATE_CLOSED: return "closed";
    case ESP_PEER_STATE_DISCONNECTED: return "disconnected";
    case ESP_PEER_STATE_NEW_CONNECTION: return "new_connection";
    case ESP_PEER_STATE_CANDIDATE_GATHERING: return "candidate_gathering";
    case ESP_PEER_STATE_PAIRING: return "pairing";
    case ESP_PEER_STATE_PAIRED: return "paired";
    case ESP_PEER_STATE_CONNECTING: return "connecting";
    case ESP_PEER_STATE_CONNECTED: return "connected";
    case ESP_PEER_STATE_CONNECT_FAILED: return "connect_failed";
    case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED: return "data_channel_connected";
    case ESP_PEER_STATE_DATA_CHANNEL_OPENED: return "data_channel_opened";
    case ESP_PEER_STATE_DATA_CHANNEL_CLOSED: return "data_channel_closed";
    case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED: return "data_channel_disconnected";
    case ESP_PEER_STATE_REMOTE_AUDIO_TRACK_ADDED: return "remote_audio_track_added";
    case ESP_PEER_STATE_REMOTE_AUDIO_TRACK_REMOVED: return "remote_audio_track_removed";
    case ESP_PEER_STATE_REMOTE_VIDEO_TRACK_ADDED: return "remote_video_track_added";
    case ESP_PEER_STATE_REMOTE_VIDEO_TRACK_REMOVED: return "remote_video_track_removed";
    case ESP_PEER_STATE_VIDEO_PLI_RECEIVED: return "video_pli_received";
    default: return "unknown";
    }
}

static int peer_state_callback(esp_peer_state_t state, void *context)
{
    (void)context;
    s_peer_state = state;
    if (state == ESP_PEER_STATE_CONNECTED) {
        s_connected = true;
        reset_keyframe_gate();
        ESP_LOGI(TAG, "peer_state=connected free_internal=%u free_psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    } else if (state == ESP_PEER_STATE_DISCONNECTED ||
               state == ESP_PEER_STATE_CONNECT_FAILED ||
               state == ESP_PEER_STATE_CLOSED) {
        s_connected = false;
        reset_keyframe_gate();
    } else if (state == ESP_PEER_STATE_VIDEO_PLI_RECEIVED) {
        reset_keyframe_gate();
        request_upstream_keyframe();
    }

    ESP_LOGI(TAG, "peer_state=%s code=%d", peer_state_name(state), state);
    if (s_signal_cb != NULL) {
        s_signal_cb("state", peer_state_name(state), s_signal_context);
    }
    return 0;
}

static int peer_message_callback(esp_peer_msg_t *message, void *context)
{
    (void)context;
    if (message == NULL || message->data == NULL || message->size <= 0) {
        ESP_LOGW(TAG, "peer_message=invalid");
        return ESP_PEER_ERR_BAD_DATA;
    }
    if (message->type == ESP_PEER_MSG_TYPE_SDP) {
        const size_t max_sdp = MEDIA_BRIDGE_SDP_MAX_BYTES - 1;
        size_t sdp_len = strnlen((const char *)message->data, max_sdp + 1);
        if (sdp_len == 0 || sdp_len > max_sdp) {
            ESP_LOGE(TAG, "local_sdp=too_large bytes=%u limit=%u",
                     (unsigned)sdp_len, (unsigned)max_sdp);
            return ESP_PEER_ERR_OVER_LIMITED;
        }
        ESP_LOGI(TAG, "local_sdp=ready bytes=%u", (unsigned)sdp_len);
        if (s_signal_cb != NULL) {
            return s_signal_cb("rtc-offer", (const char *)message->data, s_signal_context);
        }
    } else if (message->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        ESP_LOGI(TAG, "local_candidate=ready bytes=%d", message->size);
        if (s_signal_cb != NULL) {
            return s_signal_cb("rtc-candidate", (const char *)message->data, s_signal_context);
        }
    } else {
        ESP_LOGW(TAG, "peer_message=unsupported type=%d size=%d", message->type, message->size);
    }
    return 0;
}

static int peer_data_callback(esp_peer_data_frame_t *frame, void *context)
{
    (void)context;
    if (frame == NULL || frame->size < 0 || frame->size > 256) {
        ESP_LOGW(TAG, "data_channel_frame=invalid");
        return ESP_PEER_ERR_BAD_DATA;
    }
    ESP_LOGD(TAG, "data_channel_frame bytes=%d type=%d", frame->size, frame->type);
    return 0;
}

static int peer_channel_open_callback(esp_peer_data_channel_info_t *channel, void *context)
{
    (void)context;
    ESP_LOGI(TAG, "data_channel=open label=%s stream_id=%u",
             channel != NULL && channel->label != NULL ? channel->label : "(null)",
             channel != NULL ? channel->stream_id : 0);
    return 0;
}

#if CONFIG_CARPLAY_WEBRTC_TEST_STREAM
static bool read_next_test_frame(const uint8_t **data, size_t *size)
{
    const size_t stream_size = (size_t)(_binary_test_stream_bin_end - _binary_test_stream_bin_start);
    if (s_stream_offset >= stream_size) {
        s_stream_offset = 0;
    }
    if (stream_size - s_stream_offset < 4) {
        ESP_LOGE(TAG, "test_stream=truncated header offset=%u", (unsigned)s_stream_offset);
        s_stream_offset = 0;
        return false;
    }

    const uint8_t *header = _binary_test_stream_bin_start + s_stream_offset;
    size_t frame_size = ((size_t)header[0] << 24) | ((size_t)header[1] << 16) |
                        ((size_t)header[2] << 8) | (size_t)header[3];
    s_stream_offset += 4;
    if (frame_size == 0 || frame_size > stream_size - s_stream_offset) {
        ESP_LOGE(TAG, "test_stream=invalid_frame bytes=%u remaining=%u",
                 (unsigned)frame_size, (unsigned)(stream_size - s_stream_offset));
        s_stream_offset = 0;
        return false;
    }
    *data = _binary_test_stream_bin_start + s_stream_offset;
    *size = frame_size;
    s_stream_offset += frame_size;
    return true;
}

static void send_test_frame_if_due(void)
{
    if (!s_connected || s_peer == NULL || s_live_source) {
        return;
    }

    int64_t now = time_port_monotonic_us();
    if (now < s_next_frame_time_us) {
        return;
    }
    s_next_frame_time_us = now + MEDIA_BRIDGE_FRAME_PERIOD_US;

    const uint8_t *data = NULL;
    size_t size = 0;
    if (!read_next_test_frame(&data, &size)) {
        s_frames_dropped++;
        keyframe_gate_note_drop(false);
        return;
    }
    bool is_idr = frame_has_idr(data, size);
    uint32_t pts = s_video_pts;
    s_video_pts += 9000;
    if (!keyframe_gate_accepts(is_idr)) {
        s_frames_dropped++;
        keyframe_gate_note_drop(is_idr);
        return;
    }

    esp_peer_video_frame_t frame = {
        .pts = pts,
        .data = (uint8_t *)data,
        .size = (int)size,
    };
    uint32_t keyframe_generation = keyframe_gate_generation();
    if (is_idr) keyframe_gate_note_keyframe_queued(keyframe_generation);
    int result = esp_peer_send_video(s_peer, &frame);
    if (result == ESP_PEER_ERR_NONE) {
        s_frames_sent++;
        keyframe_gate_note_send(is_idr, true, keyframe_generation);
    } else {
        s_frames_dropped++;
        keyframe_gate_note_send(is_idr, false, keyframe_generation);
        if (result == ESP_PEER_ERR_WOULD_BLOCK) {
            ESP_LOGW(TAG, "video_send=backpressure frame_bytes=%u dropped=%u",
                     (unsigned)size, (unsigned)s_frames_dropped);
        } else {
            ESP_LOGW(TAG, "video_send=failed result=%d", result);
        }
    }
}
#endif

static void media_bridge_task(void *argument)
{
    (void)argument;
    uint32_t peer_loop_count = 0;
    ESP_LOGI(TAG, "peer_loop=started task_stack_bytes=%u stack_hwm_bytes=%u",
             (unsigned)MEDIA_BRIDGE_TASK_STACK_BYTES,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));

    while (!s_stop_requested) {
        answer_command_t answer = {0};
        if (xQueueReceive(s_answer_queue, &answer, 0) == pdTRUE) {
            esp_peer_msg_t message = {
                .type = ESP_PEER_MSG_TYPE_SDP,
                .data = (uint8_t *)answer.sdp,
                .size = (int)answer.size,
            };
            int result = esp_peer_send_msg(s_peer, &message);
            ESP_LOGI(TAG, "remote_sdp=applied result=%d bytes=%d", result, message.size);
            if (result != ESP_PEER_ERR_NONE && s_signal_cb != NULL) {
                s_signal_cb("error", "remote SDP rejected", s_signal_context);
            }
            release_answer_command(&answer);
        }

        int result = esp_peer_main_loop(s_peer);
        if (result != ESP_PEER_ERR_NONE && result != ESP_PEER_ERR_WOULD_BLOCK) {
            ESP_LOGW(TAG, "peer_loop=result_%d", result);
        }
        if (++peer_loop_count % 100 == 0) {
            ESP_LOGI(TAG, "peer_loop=health stack_hwm_bytes=%u free_internal=%u free_psram=%u",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        live_frame_command_t live_frame;
        if (s_live_frame_queue != NULL &&
            xQueueReceive(s_live_frame_queue, &live_frame, 0) == pdTRUE) {
            send_queued_live_frame(&live_frame);
        }
#if CONFIG_CARPLAY_WEBRTC_TEST_STREAM
        send_test_frame_if_due();
#endif
        vTaskDelay(pdMS_TO_TICKS(MEDIA_BRIDGE_LOOP_DELAY_MS));
    }

    portENTER_CRITICAL(&s_live_mux);
    media_frame_gate_stop(&s_live_frame_gate);
    s_connected = false;
    portEXIT_CRITICAL(&s_live_mux);
    for (;;) {
        portENTER_CRITICAL(&s_live_mux);
        bool producers_done = media_frame_gate_is_drained(&s_live_frame_gate);
        portEXIT_CRITICAL(&s_live_mux);
        if (producers_done) break;
        vTaskDelay(1);
    }
    s_peer_state = ESP_PEER_STATE_CLOSED;
    if (s_peer != NULL) {
        esp_peer_close(s_peer);
        s_peer = NULL;
    }
    portENTER_CRITICAL(&s_live_mux);
    s_live_buffer_busy = false;
    portEXIT_CRITICAL(&s_live_mux);
    delete_media_queues();
    heap_caps_free(s_live_buffer);
    s_live_buffer = NULL;
    s_live_buffer_capacity = 0;
    s_live_buffer_caps = 0;
    s_signal_cb = NULL;
    s_signal_context = NULL;
    s_started = false;
    s_task = NULL;
    ESP_LOGI(TAG, "peer_loop=stopped frames_sent=%u frames_dropped=%u",
             (unsigned)s_frames_sent, (unsigned)s_frames_dropped);
    xEventGroupSetBits(s_task_events, MEDIA_BRIDGE_TASK_STOPPED_BIT);
    vTaskDelete(NULL);
}

esp_err_t media_bridge_start(media_bridge_signal_cb_t signal_cb, void *context)
{
    if (signal_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task_events == NULL) {
        s_task_events = xEventGroupCreate();
        if (s_task_events == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    xEventGroupClearBits(s_task_events, MEDIA_BRIDGE_TASK_STOPPED_BIT);

    portENTER_CRITICAL(&s_live_mux);
    media_frame_gate_reset(&s_live_frame_gate, false);
    portEXIT_CRITICAL(&s_live_mux);

    QueueHandle_t answer_queue = xQueueCreate(1, sizeof(answer_command_t));
    QueueHandle_t live_frame_queue = xQueueCreate(1, sizeof(live_frame_command_t));
    if (answer_queue == NULL || live_frame_queue == NULL) {
        if (answer_queue != NULL) vQueueDelete(answer_queue);
        if (live_frame_queue != NULL) vQueueDelete(live_frame_queue);
        return ESP_ERR_NO_MEM;
    }
    s_answer_queue = answer_queue;
    portENTER_CRITICAL(&s_live_mux);
    s_live_frame_queue = live_frame_queue;
    portEXIT_CRITICAL(&s_live_mux);
    s_signal_cb = signal_cb;
    s_signal_context = context;
    s_stop_requested = false;
    s_connected = false;
    s_frames_sent = 0;
    s_frames_dropped = 0;
    reset_keyframe_gate();
    s_live_source = s_live_config_len != 0;
    media_timestamp_mapper_reset(&s_live_timestamp_mapper);
    s_live_pts_source_known = false;
    s_live_pts_source_is_sender = false;
    s_peer_state = ESP_PEER_STATE_NEW_CONNECTION;
#if CONFIG_CARPLAY_WEBRTC_TEST_STREAM
    s_stream_offset = 0;
    s_video_pts = 0;
    s_next_frame_time_us = time_port_monotonic_us();
#endif

    int result = esp_peer_pre_generate_cert();
    if (result != ESP_PEER_ERR_NONE) {
        ESP_LOGW(TAG, "dtls_certificate_cache=unavailable result=%d", result);
    }

    esp_peer_default_cfg_t defaults = {
        .agent_recv_timeout = 100,
        .data_ch_cfg = {
            .cache_timeout = 1000,
            .send_cache_size = 2048,
            .recv_cache_size = 2048,
        },
        .rtp_cfg = {
            .send_pool_size = MEDIA_BRIDGE_RTP_SEND_POOL,
            .send_queue_num = MEDIA_BRIDGE_RTP_SEND_QUEUE,
            .max_resend_count = 2,
        },
        .max_candidates = 4,
    };
    esp_peer_cfg_t config = {
        .role = ESP_PEER_ROLE_CONTROLLING,
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .video_info = {
            .codec = ESP_PEER_VIDEO_CODEC_H264,
            .width = 320,
            .height = 240,
            .fps = 10,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        /* Signaling and touch control use the authenticated WebSocket. */
        .enable_data_channel = false,
        .ctx = NULL,
        .on_state = peer_state_callback,
        .on_msg = peer_message_callback,
        .on_data = peer_data_callback,
        .on_channel_open = peer_channel_open_callback,
        .extra_cfg = &defaults,
        .extra_size = sizeof(defaults),
    };

    ESP_LOGI(TAG, "peer_open=starting free_internal=%u largest_internal=%u free_psram=%u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    result = esp_peer_open(&config, esp_peer_get_default_impl(), &s_peer);
    if (result != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "peer_open=failed result=%d meaning=%s free_internal=%u largest_internal=%u free_psram=%u",
                 result, result == ESP_PEER_ERR_NO_MEM ? "no_memory" : "other",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        delete_media_queues();
        s_signal_cb = NULL;
        s_signal_context = NULL;
        return ESP_FAIL;
    }

    s_started = true;
    if (xTaskCreate(media_bridge_task, "webrtc_peer", MEDIA_BRIDGE_TASK_STACK_BYTES,
                    NULL, MEDIA_BRIDGE_TASK_PRIORITY, &s_task) != pdPASS) {
        s_started = false;
        esp_peer_close(s_peer);
        s_peer = NULL;
        delete_media_queues();
        s_signal_cb = NULL;
        s_signal_context = NULL;
        return ESP_ERR_NO_MEM;
    }

    portENTER_CRITICAL(&s_live_mux);
    media_frame_gate_reset(&s_live_frame_gate, true);
    portEXIT_CRITICAL(&s_live_mux);

    result = esp_peer_new_connection(s_peer);
    if (result != ESP_PEER_ERR_NONE) {
        ESP_LOGE(TAG, "peer_new_connection=failed result=%d", result);
        media_bridge_stop();
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "peer_open=ready video=H264/320x240@10fps test_stream=%d", CONFIG_CARPLAY_WEBRTC_TEST_STREAM);
    return ESP_OK;
}

esp_err_t media_bridge_handle_answer(const char *sdp, size_t sdp_len)
{
    if (sdp == NULL || sdp_len == 0 || sdp_len >= MEDIA_BRIDGE_SDP_MAX_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_started || s_answer_queue == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    answer_command_t answer = {0};
    answer.sdp = heap_caps_malloc(sdp_len + 1U,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    const char *memory = "psram";
    if (answer.sdp == NULL) {
        answer.sdp = heap_caps_malloc(sdp_len + 1U,
                                      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        memory = "internal";
    }
    if (answer.sdp == NULL) {
        ESP_LOGW(TAG, "remote_sdp=allocation_failed bytes=%u free_internal=%u free_psram=%u",
                 (unsigned)sdp_len,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        return ESP_ERR_NO_MEM;
    }
    memcpy(answer.sdp, sdp, sdp_len);
    answer.sdp[sdp_len] = '\0';
    answer.size = sdp_len;
    answer_command_t replaced_answer = {0};
    bool replaced_pending =
        xQueueReceive(s_answer_queue, &replaced_answer, 0) == pdTRUE;
    release_answer_command(&replaced_answer);
    if (xQueueSend(s_answer_queue, &answer, 0) != pdPASS) {
        release_answer_command(&answer);
        ESP_LOGW(TAG, "remote_sdp=queue_full bytes=%u", (unsigned)sdp_len);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "remote_sdp=queued bytes=%u memory=%s replaced_pending=%u",
             (unsigned)sdp_len, memory, replaced_pending ? 1U : 0U);
    return ESP_OK;
}

esp_err_t media_bridge_set_h264_config(const uint8_t *annex_b_parameter_sets, size_t size)
{
    if ((annex_b_parameter_sets == NULL && size != 0) || size > sizeof(s_live_config)) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_live_mux);
    if (s_live_buffer_busy) {
        portEXIT_CRITICAL(&s_live_mux);
        return ESP_ERR_TIMEOUT;
    }
    if (size != 0) memcpy(s_live_config, annex_b_parameter_sets, size);
    s_live_config_len = size;
    s_live_source = size != 0;
    media_keyframe_gate_reset(&s_keyframe_gate);
    portEXIT_CRITICAL(&s_live_mux);
    if (s_connected) request_upstream_keyframe();
    ESP_LOGI(TAG, "live_video_config=updated parameter_set_bytes=%u", (unsigned)size);
    return ESP_OK;
}

esp_err_t media_bridge_send_h264(const uint8_t *annex_b_access_unit,
                                 size_t size,
                                 uint64_t sender_time_ns,
                                 uint64_t arrival_time_us)
{
    if (annex_b_access_unit == NULL || size == 0 ||
        size > MEDIA_BRIDGE_MAX_LIVE_FRAME_BYTES) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(false);
        return ESP_ERR_INVALID_ARG;
    }
    QueueHandle_t queue = begin_live_frame_submission();
    if (queue == NULL) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(false);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_connected || s_peer == NULL) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(false);
        end_live_frame_submission();
        return ESP_ERR_INVALID_STATE;
    }
    bool is_keyframe = frame_has_idr(annex_b_access_unit, size);
    if (!keyframe_gate_accepts(is_keyframe)) {
        ++s_frames_dropped;
        keyframe_gate_note_drop(is_keyframe);
        end_live_frame_submission();
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&s_live_mux);
    if (s_live_buffer_busy) {
        portEXIT_CRITICAL(&s_live_mux);
        ++s_frames_dropped;
        keyframe_gate_note_drop(is_keyframe);
        end_live_frame_submission();
        return ESP_ERR_TIMEOUT;
    }
    s_live_buffer_busy = true;
    size_t config_len = is_keyframe ? s_live_config_len : 0;
    s_live_source = true;
    portEXIT_CRITICAL(&s_live_mux);

    size_t frame_size = config_len + size;
    if (frame_size > MEDIA_BRIDGE_MAX_LIVE_FRAME_BYTES || !grow_live_buffer(frame_size)) {
        portENTER_CRITICAL(&s_live_mux);
        s_live_buffer_busy = false;
        portEXIT_CRITICAL(&s_live_mux);
        ++s_frames_dropped;
        keyframe_gate_note_drop(is_keyframe);
        ESP_LOGW(TAG, "live_video_buffer=unavailable bytes=%u", (unsigned)frame_size);
        end_live_frame_submission();
        return ESP_ERR_NO_MEM;
    }
    if (config_len != 0) memcpy(s_live_buffer, s_live_config, config_len);
    memcpy(s_live_buffer + config_len, annex_b_access_unit, size);
    bool used_sender_clock = false;
    uint32_t pts_90khz = media_timestamp_mapper_map(
        &s_live_timestamp_mapper, sender_time_ns, arrival_time_us,
        &used_sender_clock);
    live_frame_command_t command = {
        .size = frame_size,
        .pts = pts_90khz,
        .keyframe_generation = keyframe_gate_generation(),
        .is_keyframe = is_keyframe,
    };
    if (!s_live_pts_source_known ||
        s_live_pts_source_is_sender != used_sender_clock) {
        s_live_pts_source_known = true;
        s_live_pts_source_is_sender = used_sender_clock;
        ESP_LOGI(TAG, "live_video_pts_source=%s",
                 used_sender_clock ? "airplay_sender" : "local_arrival");
    }
    if (is_keyframe) keyframe_gate_note_keyframe_queued(command.keyframe_generation);
    if (xQueueSend(queue, &command, 0) != pdPASS) {
        portENTER_CRITICAL(&s_live_mux);
        s_live_buffer_busy = false;
        portEXIT_CRITICAL(&s_live_mux);
        ++s_frames_dropped;
        keyframe_gate_note_queue_failed(is_keyframe,
                                        command.keyframe_generation);
        end_live_frame_submission();
        return ESP_ERR_TIMEOUT;
    }
    end_live_frame_submission();
    return ESP_OK;
}

void media_bridge_stop(void)
{
    if (!s_started || s_task_events == NULL) {
        return;
    }
    s_stop_requested = true;
    (void)xEventGroupWaitBits(s_task_events, MEDIA_BRIDGE_TASK_STOPPED_BIT,
                              pdFALSE, pdTRUE, pdMS_TO_TICKS(2000));
}

bool media_bridge_is_connected(void)
{
    return s_started && s_connected && !s_stop_requested;
}

uint32_t media_bridge_frames_sent(void)
{
    return s_frames_sent;
}

uint32_t media_bridge_frames_dropped(void)
{
    return s_frames_dropped;
}

const char *media_bridge_state(void)
{
    return peer_state_name(s_peer_state);
}
