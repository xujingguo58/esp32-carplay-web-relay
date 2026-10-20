#include "web_gateway.h"

#include <math.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif_ip_addr.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#include "media_bridge.h"
#include "net_port.h"
#include "input_bridge.h"
#include "airplay_control_port.h"
#include "web_gateway_pairing.h"
#include "web_gateway_rate_limit.h"
#include "time_port.h"

#define WS_MAX_MESSAGE_BYTES 4096

static const char *TAG = "web_gateway";
static httpd_handle_t s_server;
static int s_websocket_fd = -1;
static bool s_websocket_authenticated;
static portMUX_TYPE s_websocket_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE s_touch_watchdog_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_touch_echo_count;
static input_bridge_t *s_input_bridge;
static esp_timer_handle_t s_touch_watchdog_timer;
static bool s_touch_watchdog_work_queued;
static bool s_touch_release_pending;
static web_gateway_rate_limiter_t s_websocket_rate_limiter;
static bool s_websocket_rate_limited;
static web_gateway_pairing_t s_pairing;
static char s_pairing_code[WEB_GATEWAY_PAIRING_CODE_LENGTH + 1];
static uint16_t s_display_width_pixels = 320U;
static uint16_t s_display_height_pixels = 240U;
static uint16_t s_display_rotation_degrees;
static bool s_display_geometry_from_airplay;

static void touch_watchdog_work(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_touch_watchdog_mux);
    bool release_pending = s_touch_release_pending;
    s_touch_release_pending = false;
    portEXIT_CRITICAL(&s_touch_watchdog_mux);
    if (release_pending && s_input_bridge != NULL) {
        input_bridge_release_all(s_input_bridge);
    }
    if (s_input_bridge != NULL) {
        input_bridge_tick(s_input_bridge, (uint64_t)time_port_monotonic_ms());
    }
    portENTER_CRITICAL(&s_touch_watchdog_mux);
    s_touch_watchdog_work_queued = false;
    portEXIT_CRITICAL(&s_touch_watchdog_mux);
}

static void touch_release_work(void *context)
{
    (void)context;
    portENTER_CRITICAL(&s_touch_watchdog_mux);
    s_touch_release_pending = false;
    portEXIT_CRITICAL(&s_touch_watchdog_mux);
    if (s_input_bridge != NULL) input_bridge_release_all(s_input_bridge);
}

static void touch_watchdog_timer_callback(void *context)
{
    (void)context;
    httpd_handle_t server = s_server;
    if (server == NULL) return;

    portENTER_CRITICAL(&s_touch_watchdog_mux);
    if (s_touch_watchdog_work_queued) {
        portEXIT_CRITICAL(&s_touch_watchdog_mux);
        return;
    }
    s_touch_watchdog_work_queued = true;
    portEXIT_CRITICAL(&s_touch_watchdog_mux);

    esp_err_t err = httpd_queue_work(server, touch_watchdog_work, NULL);
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_touch_watchdog_mux);
        s_touch_watchdog_work_queued = false;
        portEXIT_CRITICAL(&s_touch_watchdog_mux);
        ESP_LOGD(TAG, "touch_watchdog=queue_failed error=%s", esp_err_to_name(err));
    }
}

static bool input_report_callback(uint32_t hid_uid, const uint8_t *report,
                                 size_t report_len, void *context)
{
    return airplay_control_port_send_hid_report(hid_uid, report, report_len, context);
}

static const char INDEX_HTML[] =
    "<!doctype html><html lang=\"zh-CN\"><head>"
    "<meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>ESP32 CarPlay Web Relay · M2</title><style>"
    "*{box-sizing:border-box}body{margin:0;background:#101820;color:#edf2f4;font:16px system-ui,sans-serif}"
    "header{padding:12px 18px;background:#1d2b34;display:flex;gap:16px;align-items:center;flex-wrap:wrap}"
    "header strong{font-size:18px}#state{color:#b8d8c0}main{max-width:1000px;margin:18px auto;padding:0 14px}"
    "#stage{position:relative;background:#05090d;width:100%;aspect-ratio:4/3;max-height:72vh;display:grid;place-items:center}"
    "video{width:100%;height:100%;object-fit:contain;touch-action:none}#overlay{position:absolute;pointer-events:none;color:#aebdc5;text-align:center}"
    "section{margin-top:14px;padding:14px;border-radius:10px;background:#1d2b34}button,input{padding:9px 14px;border:0;"
    "border-radius:7px;background:#93d3ac;color:#11231a;font-weight:650}small,pre{color:#b6c6cd}pre{white-space:pre-wrap;word-break:break-word}"
    "input{background:#edf2f4;max-width:12em;margin-right:8px}"
    "</style></head><body><header><strong>ESP32 CarPlay Web Relay · M2 样机</strong>"
    "<span id=\"state\">WebSocket 连接中…</span><button id=\"reconnect\">重新连接</button></header>"
    "<main><div id=\"stage\"><video id=\"video\" autoplay playsinline muted></video>"
    "<div id=\"overlay\">等待 WebRTC H.264 合成测试流<br><small>此画面不是 CarPlay 实际接收结果</small></div></div>"
    "<section id=\"pairing\"><b>浏览器会话配对</b><p>输入设备本次启动时从串口输出的 8 位临时码。</p>"
    "<input id=\"pair-code\" type=\"password\" maxlength=\"8\" autocomplete=\"off\" inputmode=\"text\" aria-label=\"配对码\">"
    "<button id=\"pair-submit\">配对并连接</button> <small id=\"pair-hint\">未配对前不会启动媒体或触控通道</small></section>"
    "<section><b>测试触控回显</b><p>在视频区域按下并移动指针，验证浏览器到 ESP32 的消息往返。</p>"
    "<div>已回显事件：<span id=\"touch-count\">0</span></div><pre id=\"touch-last\">尚无触控事件</pre></section>"
    "<section><b>设备指标</b><pre id=\"metrics\">读取中…</pre></section></main><script>"
    "const state=document.querySelector('#state'),video=document.querySelector('#video'),overlay=document.querySelector('#overlay');"
    "const pairing=document.querySelector('#pairing'),pairCode=document.querySelector('#pair-code'),pairHint=document.querySelector('#pair-hint');"
    "const touchCount=document.querySelector('#touch-count'),touchLast=document.querySelector('#touch-last');let ws,pc,echoes=0,pendingCandidates=[];"
    "const activePointers=new Set(),lastMoves=new Map();"
    "function setState(s){state.textContent=s}function send(o){if(ws&&ws.readyState===WebSocket.OPEN)ws.send(JSON.stringify(o))}"
    "function closePeer(){if(pc){pc.close();pc=null}pendingCandidates=[];video.srcObject=null;overlay.hidden=false}"
    "function waitIce(p){return p.iceGatheringState==='complete'?Promise.resolve():new Promise(r=>{"
    "const f=()=>{if(p.iceGatheringState==='complete'){p.removeEventListener('icegatheringstatechange',f);r()}};"
    "p.addEventListener('icegatheringstatechange',f);setTimeout(r,8000)})}"
    "function answerSetupForOffer(offerSdp,answerSdp){const setupLines=offerSdp.match(/^a=setup:[^\\r\\n]*\\r?$/gm)||[];"
    "if(!setupLines.length||setupLines.some(line=>!line.startsWith('a=setup:actpass')))return answerSdp;"
    "return answerSdp.replace(/^a=setup:active(\\r?)$/gm,'a=setup:passive$1')}"
    "function normalizedVideoPoint(clientX,clientY,rect,videoWidth,videoHeight,phase){if(!rect.width||!rect.height)return null;"
    "const vw=videoWidth||rect.width,vh=videoHeight||rect.height,scale=Math.min(rect.width/vw,rect.height/vh);"
    "const w=vw*scale,h=vh*scale,left=rect.left+(rect.width-w)/2,top=rect.top+(rect.height-h)/2;"
    "if(!w||!h)return null;const outside=clientX<left||clientX>left+w||clientY<top||clientY>top+h;"
    "if(outside&&phase==='down')return null;return{x:Math.max(0,Math.min(1,(clientX-left)/w)),y:Math.max(0,Math.min(1,(clientY-top)/h))}}"
    "async function offer(sdp){closePeer();pc=new RTCPeerConnection({iceServers:[]});const p=pc;"
    "p.ontrack=e=>{video.srcObject=e.streams[0]||new MediaStream([e.track]);video.play().catch(()=>{});overlay.hidden=true};"
    "p.onconnectionstatechange=()=>setState('WebRTC '+p.connectionState);"
    "await p.setRemoteDescription({type:'offer',sdp});while(pendingCandidates.length)await p.addIceCandidate(pendingCandidates.shift());const a=await p.createAnswer();"
    "a.sdp=answerSetupForOffer(sdp,a.sdp);await p.setLocalDescription(a);await waitIce(p);"
    "if(ws&&ws.readyState===WebSocket.OPEN)send({type:'rtc-answer',sdp:p.localDescription.sdp})}"
    "function connect(){closePeer();if(ws)ws.close();const scheme=location.protocol==='https:'?'wss':'ws';"
    "ws=new WebSocket(scheme+'://'+location.host+'/ws');ws.onopen=()=>setState('信令已连接，请输入串口配对码');"
    "ws.onclose=()=>{closePeer();activePointers.clear();lastMoves.clear();pairing.hidden=false;pairHint.textContent='连接已关闭，请重新配对';setState('信令断开')};ws.onerror=()=>setState('信令错误');"
    "ws.onmessage=async e=>{let m;try{m=JSON.parse(e.data)}catch(_){return}"
    "if(m.type==='paired'){pairing.hidden=true;pairCode.value='';setState('配对成功，等待 WebRTC');}"
    "else if(m.type==='pair-error'){pairing.hidden=false;pairHint.textContent='配对失败：'+m.reason}"
    "if(m.type==='rtc-offer'){try{await offer(m.sdp)}catch(err){setState('WebRTC 协商失败：'+err)}}"
    "else if(m.type==='rtc-candidate'&&typeof m.payload==='string'&&pc){const c={candidate:m.payload,sdpMLineIndex:0};"
    "if(pc.remoteDescription)pc.addIceCandidate(c).catch(err=>setState('ICE candidate 失败：'+err));else pendingCandidates.push(c)}"
    "else if(m.type==='state')setState('Peer：'+m.payload);else if(m.type==='error')setState('错误：'+m.payload);"
    "else if(m.type==='rtc-answer-accepted')setState('Answer 已交给 ESP32');"
    "else if(m.type==='touch-echo'){echoes++;touchCount.textContent=echoes;touchLast.textContent=JSON.stringify(m)}"
    "else if(m.type==='touch-error')touchLast.textContent='触控消息拒绝：'+m.reason}}"
    "document.querySelector('#pair-submit').onclick=()=>send({type:'pair',code:pairCode.value.trim().toLowerCase()});"
    "pairCode.addEventListener('keydown',e=>{if(e.key==='Enter')document.querySelector('#pair-submit').click()});"
    "document.querySelector('#reconnect').onclick=connect;connect();"
    "const stage=document.querySelector('#stage');"
    "function point(e,phase){const p=normalizedVideoPoint(e.clientX,e.clientY,video.getBoundingClientRect(),"
    "video.videoWidth,video.videoHeight,phase);if(!p)return false;send({type:'touch',phase,id:e.pointerId,x:p.x,y:p.y});return true}"
    "stage.addEventListener('pointerdown',e=>{if(activePointers.size>=2||!point(e,'down'))return;"
    "activePointers.add(e.pointerId);lastMoves.set(e.pointerId,performance.now());stage.setPointerCapture(e.pointerId)});"
    "stage.addEventListener('pointermove',e=>{if(!activePointers.has(e.pointerId))return;const now=performance.now();"
    "if(now-(lastMoves.get(e.pointerId)||0)>=30){lastMoves.set(e.pointerId,now);point(e,'move')}});"
    "function release(e,phase){if(activePointers.delete(e.pointerId)){lastMoves.delete(e.pointerId);point(e,phase)}}"
    "stage.addEventListener('pointerup',e=>release(e,'up'));stage.addEventListener('pointercancel',e=>release(e,'cancel'));"
    "stage.addEventListener('lostpointercapture',e=>release(e,'cancel'));"
    "function cancelActivePointers(){for(const id of activePointers)send({type:'touch',phase:'cancel',id,x:0,y:0});"
    "activePointers.clear();lastMoves.clear()}addEventListener('blur',cancelActivePointers);"
    "async function refresh(){try{const r=await fetch('/api/status',{cache:'no-store'}),data={esp32:await r.json()};"
    "if(pc){const stats=await pc.getStats();let inbound=null,rtt=null;stats.forEach(s=>{"
    "if(s.type==='inbound-rtp'&&(s.kind==='video'||s.mediaType==='video'))inbound=s;"
    "if(s.type==='candidate-pair'&&s.state==='succeeded'&&s.nominated&&Number.isFinite(s.currentRoundTripTime))rtt=s.currentRoundTripTime*1000});"
    "data.browser={framesPerSecond:inbound?.framesPerSecond??null,framesDecoded:inbound?.framesDecoded??null,"
    "framesDropped:inbound?.framesDropped??null,packetsLost:inbound?.packetsLost??null,bytesReceived:inbound?.bytesReceived??null,rttMs:rtt}}"
    "document.querySelector('#metrics').textContent=JSON.stringify(data,null,2)}catch(_){}}"
    "refresh();setInterval(refresh,2000);</script></body></html>";

static bool websocket_is_active(int fd)
{
    bool active;
    portENTER_CRITICAL(&s_websocket_mux);
    active = s_websocket_fd == fd;
    portEXIT_CRITICAL(&s_websocket_mux);
    return active;
}

static void async_send_complete(esp_err_t error, int socket, void *argument)
{
    (void)socket;
    if (error != ESP_OK) {
        ESP_LOGD(TAG, "websocket_send=failed error=%s", esp_err_to_name(error));
    }
    free(argument);
}

static esp_err_t websocket_send_text_async(int fd, const char *text)
{
    if (s_server == NULL || text == NULL || !websocket_is_active(fd) ||
        httpd_ws_get_fd_info(s_server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) {
        return ESP_ERR_INVALID_STATE;
    }
    size_t len = strlen(text);
    uint8_t *payload = malloc(len + 1);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(payload, text, len + 1);
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = payload,
        .len = len,
    };
    esp_err_t err = httpd_ws_send_data_async(s_server, fd, &frame, async_send_complete, payload);
    if (err != ESP_OK) {
        free(payload);
    }
    return err;
}

static esp_err_t media_signal_callback(const char *type, const char *payload, void *context)
{
    int fd = (int)(intptr_t)context;
    if (type == NULL || payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(type, "keyframe-request") == 0) {
        bool requested = airplay_control_port_request_main_screen_keyframe();
        if (!requested) {
            ESP_LOGD(TAG, "airplay_keyframe_request=unavailable");
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_OK;
    }
    cJSON *message = cJSON_CreateObject();
    if (message == NULL) {
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(message, "type", type);
    if (strcmp(type, "rtc-offer") == 0) {
        cJSON_AddStringToObject(message, "sdp", payload);
    } else {
        cJSON_AddStringToObject(message, "payload", payload);
    }
    char *encoded = cJSON_PrintUnformatted(message);
    cJSON_Delete(message);
    if (encoded == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = websocket_send_text_async(fd, encoded);
    cJSON_free(encoded);
    return err;
}

static esp_err_t index_get_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t status_get_handler(httpd_req_t *request)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *ap_netif = net_port_get_ap_netif();
    if (ap_netif == NULL || esp_netif_get_ip_info(ap_netif, &ip_info) != ESP_OK) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "AP not ready");
        return ESP_FAIL;
    }

    char body[512];
    int body_len = snprintf(body, sizeof(body),
                            "{\"ap_ip\":\"" IPSTR "\",\"clients\":%u,"
                            "\"free_internal_heap_bytes\":%u,\"minimum_free_internal_heap_bytes\":%u,"
                            "\"free_psram_bytes\":%u,\"minimum_free_psram_bytes\":%u,"
                            "\"touch_display_width\":%u,\"touch_display_height\":%u,"
                            "\"touch_display_rotation\":%u,"
                            "\"webrtc_state\":\"%s\",\"webrtc_connected\":%s,"
                            "\"video_frames_sent\":%u,\"video_frames_dropped\":%u,"
                            "\"touch_echo_count\":%u,\"touch_active_contacts\":%u,"
                            "\"touch_invalid_events\":%u}",
                            IP2STR(&ip_info.ip), (unsigned)net_port_station_count(),
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
                            (unsigned)s_display_width_pixels,
                            (unsigned)s_display_height_pixels,
                            (unsigned)s_display_rotation_degrees,
                            media_bridge_state(), media_bridge_is_connected() ? "true" : "false",
                            (unsigned)media_bridge_frames_sent(),
                            (unsigned)media_bridge_frames_dropped(),
                            (unsigned)s_touch_echo_count,
                            (unsigned)input_bridge_active_contacts(s_input_bridge),
                            (unsigned)input_bridge_invalid_events(s_input_bridge));
    if (body_len < 0 || (size_t)body_len >= sizeof(body)) {
        httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Status too large");
        return ESP_FAIL;
    }

    httpd_resp_set_type(request, "application/json; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    return httpd_resp_send(request, body, body_len);
}

static bool valid_touch_message(const cJSON *message, const char **phase, int *pointer_id,
                                double *x, double *y)
{
    const cJSON *phase_item = cJSON_GetObjectItemCaseSensitive(message, "phase");
    const cJSON *id_item = cJSON_GetObjectItemCaseSensitive(message, "id");
    const cJSON *x_item = cJSON_GetObjectItemCaseSensitive(message, "x");
    const cJSON *y_item = cJSON_GetObjectItemCaseSensitive(message, "y");
    if (!cJSON_IsString(phase_item) || !cJSON_IsNumber(id_item) ||
        !cJSON_IsNumber(x_item) || !cJSON_IsNumber(y_item)) {
        return false;
    }
    if (strcmp(phase_item->valuestring, "down") != 0 &&
        strcmp(phase_item->valuestring, "move") != 0 &&
        strcmp(phase_item->valuestring, "up") != 0 &&
        strcmp(phase_item->valuestring, "cancel") != 0) {
        return false;
    }
    if (!isfinite(id_item->valuedouble) || id_item->valuedouble < 0 || id_item->valuedouble > 65535 ||
        floor(id_item->valuedouble) != id_item->valuedouble ||
        !isfinite(x_item->valuedouble) || !isfinite(y_item->valuedouble) ||
        x_item->valuedouble < 0 || x_item->valuedouble > 1 ||
        y_item->valuedouble < 0 || y_item->valuedouble > 1) {
        return false;
    }
    *phase = phase_item->valuestring;
    *pointer_id = (int)id_item->valuedouble;
    *x = x_item->valuedouble;
    *y = y_item->valuedouble;
    return true;
}

static esp_err_t send_touch_echo(httpd_req_t *request, const char *phase,
                                 int pointer_id, double x, double y, bool hid_connected)
{
    char response[192];
    int response_len = snprintf(response, sizeof(response),
                                "{\"type\":\"touch-echo\",\"phase\":\"%s\","
                                "\"id\":%d,\"x\":%.5f,\"y\":%.5f,\"hid\":\"%s\"}",
                                phase, pointer_id, x, y,
                                hid_connected ? "sent" : "not_connected");
    if (response_len < 0 || (size_t)response_len >= sizeof(response)) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)response,
        .len = (size_t)response_len,
    };
    esp_err_t err = httpd_ws_send_frame(request, &frame);
    if (err == ESP_OK) {
        s_touch_echo_count++;
        ESP_LOGI(TAG, "touch_echo phase=%s id=%d x=%.3f y=%.3f hid_connected=%d count=%u",
                 phase, pointer_id, x, y, hid_connected, (unsigned)s_touch_echo_count);
    }
    return err;
}

static esp_err_t send_touch_error(httpd_req_t *request, const char *reason)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
                       "{\"type\":\"touch-error\",\"reason\":\"%s\"}", reason);
    if (len < 0 || (size_t)len >= sizeof(response)) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)response,
        .len = (size_t)len,
    };
    return httpd_ws_send_frame(request, &frame);
}

static esp_err_t send_pair_error(httpd_req_t *request, const char *reason)
{
    char response[128];
    int len = snprintf(response, sizeof(response),
                       "{\"type\":\"pair-error\",\"reason\":\"%s\"}", reason);
    if (len < 0 || (size_t)len >= sizeof(response)) {
        return ESP_FAIL;
    }
    httpd_ws_frame_t frame = {
        .type = HTTPD_WS_TYPE_TEXT,
        .payload = (uint8_t *)response,
        .len = (size_t)len,
    };
    return httpd_ws_send_frame(request, &frame);
}

static esp_err_t websocket_handler(httpd_req_t *request)
{
    int fd = httpd_req_to_sockfd(request);
    if (request->method == HTTP_GET) {
        bool available;
        portENTER_CRITICAL(&s_websocket_mux);
        available = s_websocket_fd < 0;
        if (available) {
            s_websocket_fd = fd;
            s_websocket_authenticated = false;
            s_websocket_rate_limited = false;
            web_gateway_rate_limiter_reset(&s_websocket_rate_limiter,
                                           (uint64_t)time_port_monotonic_ms());
        }
        portEXIT_CRITICAL(&s_websocket_mux);
        if (!available) {
            httpd_resp_set_status(request, "409 Conflict");
            httpd_resp_sendstr(request, "Only one browser media session is supported");
            return ESP_FAIL;
        }
        s_touch_echo_count = 0;
        ESP_LOGI(TAG, "websocket_session=opened fd=%d awaiting_pairing=1", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t packet = {0};
    esp_err_t err = httpd_ws_recv_frame(request, &packet, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "websocket_receive=length_failed error=%s", esp_err_to_name(err));
        return err;
    }
    if (packet.type == HTTPD_WS_TYPE_CLOSE) {
        s_websocket_authenticated = false;
        input_bridge_release_all(s_input_bridge);
        media_bridge_stop();
        return ESP_OK;
    }
    if (s_websocket_rate_limited) return ESP_OK;
    uint64_t now_ms = (uint64_t)time_port_monotonic_ms();
    if (!web_gateway_rate_limiter_allow(&s_websocket_rate_limiter, now_ms)) {
        s_websocket_rate_limited = true;
        ESP_LOGW(TAG, "websocket_rate_limit=exceeded fd=%d limit_per_second=%u burst=%u",
                 fd, WEB_GATEWAY_RATE_LIMIT_PER_SECOND, WEB_GATEWAY_RATE_LIMIT_BURST);
        input_bridge_release_all(s_input_bridge);
        if (s_server != NULL) {
            esp_err_t close_err = httpd_sess_trigger_close(s_server, fd);
            if (close_err != ESP_OK) {
                ESP_LOGW(TAG, "websocket_rate_limit=close_failed fd=%d error=%s",
                         fd, esp_err_to_name(close_err));
            }
        }
        return ESP_OK;
    }
    if (packet.type != HTTPD_WS_TYPE_TEXT || packet.len == 0 || packet.len > WS_MAX_MESSAGE_BYTES) {
        ESP_LOGW(TAG, "websocket_receive=rejected type=%d bytes=%u", packet.type, (unsigned)packet.len);
        return ESP_ERR_INVALID_ARG;
    }

    char *payload = calloc(1, packet.len + 1);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    packet.payload = (uint8_t *)payload;
    err = httpd_ws_recv_frame(request, &packet, packet.len);
    if (err != ESP_OK) {
        free(payload);
        return err;
    }
    cJSON *message = cJSON_ParseWithLength(payload, packet.len);
    free(payload);
    if (message == NULL) {
        return send_touch_error(request, "invalid_json");
    }

    const cJSON *type_item = cJSON_GetObjectItemCaseSensitive(message, "type");
    if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "pair") == 0) {
        const cJSON *code_item = cJSON_GetObjectItemCaseSensitive(message, "code");
        uint64_t now_ms = (uint64_t)time_port_monotonic_ms();
        if (web_gateway_pairing_is_locked(&s_pairing, now_ms)) {
            err = send_pair_error(request, "locked_try_later");
        } else if (!cJSON_IsString(code_item) || code_item->valuestring == NULL) {
            err = send_pair_error(request, "invalid_code");
        } else if (!web_gateway_pairing_verify(&s_pairing, code_item->valuestring,
                                               strlen(code_item->valuestring), now_ms)) {
            err = send_pair_error(request,
                                  web_gateway_pairing_is_locked(&s_pairing, now_ms)
                                      ? "locked_try_later"
                                      : "invalid_code");
        } else {
            s_websocket_authenticated = true;
            err = websocket_send_text_async(fd, "{\"type\":\"paired\"}");
            if (err == ESP_OK) {
                err = media_bridge_start(media_signal_callback, (void *)(intptr_t)fd);
                if (err != ESP_OK) {
                    s_websocket_authenticated = false;
                    ESP_LOGE(TAG, "webrtc_session=start_failed error=%s", esp_err_to_name(err));
                    err = send_pair_error(request, "media_start_failed");
                }
            } else {
                s_websocket_authenticated = false;
            }
        }
    } else if (!s_websocket_authenticated) {
        err = send_pair_error(request, "pairing_required");
    } else if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "rtc-answer") == 0) {
        const cJSON *sdp_item = cJSON_GetObjectItemCaseSensitive(message, "sdp");
        if (!cJSON_IsString(sdp_item)) {
            err = ESP_ERR_INVALID_ARG;
        } else {
            err = media_bridge_handle_answer(sdp_item->valuestring, strlen(sdp_item->valuestring));
        }
        if (err == ESP_OK) {
            err = websocket_send_text_async(fd, "{\"type\":\"rtc-answer-accepted\"}");
        } else {
            ESP_LOGW(TAG, "remote_sdp=rejected error=%s", esp_err_to_name(err));
            err = send_touch_error(request, "invalid_answer");
        }
    } else if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "touch") == 0) {
        const char *phase = NULL;
        int pointer_id = 0;
        double x = 0;
        double y = 0;
        if (valid_touch_message(message, &phase, &pointer_id, &x, &y)) {
            input_bridge_phase_t input_phase = INPUT_BRIDGE_TOUCH_DOWN;
            if (strcmp(phase, "move") == 0) input_phase = INPUT_BRIDGE_TOUCH_MOVE;
            else if (strcmp(phase, "up") == 0) input_phase = INPUT_BRIDGE_TOUCH_UP;
            else if (strcmp(phase, "cancel") == 0) input_phase = INPUT_BRIDGE_TOUCH_CANCEL;
            input_bridge_result_t result = input_bridge_touch(
                s_input_bridge, input_phase, (uint16_t)pointer_id, x, y,
                (uint64_t)time_port_monotonic_ms());
            if (result == INPUT_BRIDGE_INVALID_ARGUMENT ||
                result == INPUT_BRIDGE_NO_CONTACT_SLOT ||
                result == INPUT_BRIDGE_UNKNOWN_CONTACT) {
                err = send_touch_error(request, "touch_state_rejected");
            } else {
                err = send_touch_echo(request, phase, pointer_id, x, y,
                                      result == INPUT_BRIDGE_OK);
            }
        } else {
            err = send_touch_error(request, "invalid_touch");
        }
    } else {
        err = send_touch_error(request, "unsupported_message");
    }
    cJSON_Delete(message);
    return err;
}

static void websocket_close_callback(httpd_handle_t server, int socket)
{
    (void)server;
    bool was_active = false;
    portENTER_CRITICAL(&s_websocket_mux);
    if (s_websocket_fd == socket) {
        s_websocket_fd = -1;
        s_websocket_authenticated = false;
        s_websocket_rate_limited = false;
        web_gateway_rate_limiter_reset(&s_websocket_rate_limiter,
                                       (uint64_t)time_port_monotonic_ms());
        was_active = true;
    }
    portEXIT_CRITICAL(&s_websocket_mux);
    if (was_active) {
        ESP_LOGI(TAG, "websocket_session=closed fd=%d", socket);
        input_bridge_release_all(s_input_bridge);
        media_bridge_stop();
    }
    close(socket);
}

esp_err_t web_gateway_set_display_geometry(uint16_t width_pixels,
                                           uint16_t height_pixels,
                                           uint16_t rotation_degrees)
{
    if (width_pixels == 0U || height_pixels == 0U ||
        (rotation_degrees != 0U && rotation_degrees != 90U &&
         rotation_degrees != 180U && rotation_degrees != 270U)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_server != NULL || s_input_bridge != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    s_display_width_pixels = width_pixels;
    s_display_height_pixels = height_pixels;
    s_display_rotation_degrees = rotation_degrees;
    s_display_geometry_from_airplay = true;
    return ESP_OK;
}

esp_err_t web_gateway_release_touches(void)
{
    httpd_handle_t server = s_server;
    if (server == NULL || s_input_bridge == NULL) return ESP_ERR_INVALID_STATE;
    portENTER_CRITICAL(&s_touch_watchdog_mux);
    s_touch_release_pending = true;
    portEXIT_CRITICAL(&s_touch_watchdog_mux);
    return httpd_queue_work(server, touch_release_work, NULL);
}

esp_err_t web_gateway_start(void)
{
    uint32_t random_code = esp_random();
    (void)snprintf(s_pairing_code, sizeof(s_pairing_code), "%08" PRIx32, random_code);
    if (!web_gateway_pairing_init(&s_pairing, s_pairing_code)) {
        ESP_LOGE(TAG, "web_pairing=init_failed");
        return ESP_FAIL;
    }

    esp_err_t err = airplay_control_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "airplay_control_port=init_failed error=%s", esp_err_to_name(err));
        return err;
    }
    if (s_input_bridge == NULL) {
        s_input_bridge = input_bridge_create(s_display_width_pixels,
                                             s_display_height_pixels,
                                             s_display_rotation_degrees,
                                             input_report_callback, NULL);
        if (s_input_bridge == NULL) {
            ESP_LOGE(TAG, "input_bridge=create_failed");
            return ESP_ERR_NO_MEM;
        }
    }
    ESP_LOGI(TAG, "touch_display=ready width=%u height=%u rotation=%u source=%s",
             (unsigned)s_display_width_pixels, (unsigned)s_display_height_pixels,
             (unsigned)s_display_rotation_degrees,
             s_display_geometry_from_airplay ? "airplay_info" : "m2_sample");
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 4;
    config.max_open_sockets = 4;
    config.stack_size = 8192;
    config.close_fn = websocket_close_callback;

    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t index_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = index_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t websocket_uri = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = websocket_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
    };

    err = httpd_register_uri_handler(s_server, &index_uri);
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &status_uri);
    }
    if (err == ESP_OK) {
        err = httpd_register_uri_handler(s_server, &websocket_uri);
    }
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = touch_watchdog_timer_callback,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "touch_watchdog",
    };
    err = esp_timer_create(&timer_args, &s_touch_watchdog_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_touch_watchdog_timer, 250000U);
    }
    if (err != ESP_OK) {
        if (s_touch_watchdog_timer != NULL) {
            (void)esp_timer_delete(s_touch_watchdog_timer);
            s_touch_watchdog_timer = NULL;
        }
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    ESP_LOGW(TAG, "web_pairing_code=%s scope=boot_only transport=UART", s_pairing_code);
    ESP_LOGI(TAG, "HTTP/WebSocket server listening on port %u touch_watchdog_ms=250",
             config.server_port);
    return ESP_OK;
}
