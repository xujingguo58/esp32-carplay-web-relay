#pragma once

static inline void esp_host_log(const char *format, ...)
{ (void)format; }

#define ESP_LOGD(tag, ...) do { (void)(tag); if (0) esp_host_log(__VA_ARGS__); } while (0)
#define ESP_LOGI(tag, ...) do { (void)(tag); if (0) esp_host_log(__VA_ARGS__); } while (0)
#define ESP_LOGW(tag, ...) do { (void)(tag); if (0) esp_host_log(__VA_ARGS__); } while (0)
#define ESP_LOGE(tag, ...) do { (void)(tag); if (0) esp_host_log(__VA_ARGS__); } while (0)
