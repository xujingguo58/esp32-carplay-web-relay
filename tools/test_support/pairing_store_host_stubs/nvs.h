#pragma once

#include <stddef.h>

#include "esp_err.h"

typedef int nvs_handle_t;
typedef enum {
    NVS_READONLY = 0,
    NVS_READWRITE = 1,
} nvs_open_mode_t;

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle);
void nvs_close(nvs_handle_t handle);
esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length);
esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length);
esp_err_t nvs_commit(nvs_handle_t handle);

void nvs_mock_reset(void);
void nvs_mock_fail_next_commit(void);
void nvs_mock_corrupt(size_t offset, unsigned char value);
void nvs_mock_set_size(size_t size);
