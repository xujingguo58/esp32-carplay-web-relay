#include "net_port.h"

#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

static const char *TAG = "net_port";

static esp_netif_t *s_ap_netif;
static uint8_t s_station_count;
static portMUX_TYPE s_station_count_lock = portMUX_INITIALIZER_UNLOCKED;

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        portENTER_CRITICAL(&s_station_count_lock);
        if (s_station_count < UINT8_MAX) {
            ++s_station_count;
        }
        uint8_t connected = s_station_count;
        portEXIT_CRITICAL(&s_station_count_lock);
        ESP_LOGI(TAG, "Client connected; clients=%u", (unsigned)connected);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        portENTER_CRITICAL(&s_station_count_lock);
        if (s_station_count > 0) {
            --s_station_count;
        }
        uint8_t connected = s_station_count;
        portEXIT_CRITICAL(&s_station_count_lock);
        ESP_LOGI(TAG, "Client disconnected; clients=%u", (unsigned)connected);
    }
}

esp_err_t net_port_start(void)
{
    const size_t ssid_len = strlen(CONFIG_CARPLAY_AP_SSID);
    const size_t password_len = strlen(CONFIG_CARPLAY_AP_PASSWORD);

    if (ssid_len == 0 || ssid_len > 32) {
        ESP_LOGE(TAG, "SoftAP SSID must contain 1 to 32 bytes");
        return ESP_ERR_INVALID_ARG;
    }
    if (password_len < 8 || password_len > 63) {
        ESP_LOGE(TAG,
                 "Set a WPA2 password of 8 to 63 characters in menuconfig "
                 "(CarPlay Relay -> SoftAP)");
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        return err;
    }

    s_ap_netif = esp_netif_create_default_wifi_ap();
    if (s_ap_netif == NULL) {
        ESP_LOGE(TAG, "Failed to create default SoftAP network interface");
        return ESP_FAIL;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_event_handler_register(WIFI_EVENT,
                                     ESP_EVENT_ANY_ID,
                                     wifi_event_handler,
                                     NULL);
    if (err != ESP_OK) {
        return err;
    }

    wifi_config_t ap_config = {0};
    memcpy(ap_config.ap.ssid, CONFIG_CARPLAY_AP_SSID, ssid_len);
    ap_config.ap.ssid_len = (uint8_t)ssid_len;
    memcpy(ap_config.ap.password, CONFIG_CARPLAY_AP_PASSWORD, password_len);
    ap_config.ap.channel = CONFIG_CARPLAY_AP_CHANNEL;
    ap_config.ap.max_connection = CONFIG_CARPLAY_AP_MAX_STA;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap_config.ap.pmf_cfg.capable = true;
    ap_config.ap.pmf_cfg.required = false;

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return err;
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG,
             "SoftAP started: ssid=%s channel=%d max_clients=%d",
             CONFIG_CARPLAY_AP_SSID,
             CONFIG_CARPLAY_AP_CHANNEL,
             CONFIG_CARPLAY_AP_MAX_STA);
    return ESP_OK;
}

esp_netif_t *net_port_get_ap_netif(void)
{
    return s_ap_netif;
}

uint8_t net_port_station_count(void)
{
    portENTER_CRITICAL(&s_station_count_lock);
    uint8_t count = s_station_count;
    portEXIT_CRITICAL(&s_station_count_lock);
    return count;
}
