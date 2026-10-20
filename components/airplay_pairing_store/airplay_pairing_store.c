/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "airplay_pairing_store.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"

#define STORE_NAMESPACE "airplay_pair"
#define STORE_KEY "controllers"
#define STORE_MAGIC "APS1"
#define STORE_VERSION 1U
#define STORE_HEADER_BYTES 6U
#define STORE_RECORD_BYTES (1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES + \
                            AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES)
#define STORE_BLOB_BYTES (STORE_HEADER_BYTES + \
                          AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS * STORE_RECORD_BYTES)

typedef struct {
    uint8_t identifier_len;
    uint8_t identifier[AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES];
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES];
} controller_record_t;

typedef struct {
    uint8_t count;
    controller_record_t records[AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS];
} controller_table_t;

static SemaphoreHandle_t s_store_mutex;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static bool identifier_valid(const uint8_t *identifier, size_t identifier_len)
{
    return identifier != NULL && identifier_len != 0U &&
           identifier_len <= AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES;
}

static bool identifier_equal(const controller_record_t *record,
                             const uint8_t *identifier,
                             size_t identifier_len)
{
    return record->identifier_len == identifier_len &&
           memcmp(record->identifier, identifier, identifier_len) == 0;
}

static void encode_table(const controller_table_t *table,
                         uint8_t blob[STORE_BLOB_BYTES])
{
    memset(blob, 0, STORE_BLOB_BYTES);
    memcpy(blob, STORE_MAGIC, 4U);
    blob[4] = STORE_VERSION;
    blob[5] = table->count;
    for (size_t i = 0U; i < table->count; ++i) {
        size_t offset = STORE_HEADER_BYTES + i * STORE_RECORD_BYTES;
        const controller_record_t *record = &table->records[i];
        blob[offset] = record->identifier_len;
        memcpy(blob + offset + 1U, record->identifier,
               AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES);
        memcpy(blob + offset + 1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES,
               record->public_key, AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
    }
}

static esp_err_t decode_table(const uint8_t blob[STORE_BLOB_BYTES],
                              controller_table_t *table)
{
    if (memcmp(blob, STORE_MAGIC, 4U) != 0 || blob[4] != STORE_VERSION ||
        blob[5] > AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS) {
        return ESP_ERR_INVALID_CRC;
    }
    memset(table, 0, sizeof(*table));
    table->count = blob[5];
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS; ++i) {
        size_t offset = STORE_HEADER_BYTES + i * STORE_RECORD_BYTES;
        uint8_t identifier_len = blob[offset];
        if (i >= table->count) {
            for (size_t j = 0U; j < STORE_RECORD_BYTES; ++j) {
                if (blob[offset + j] != 0U) return ESP_ERR_INVALID_CRC;
            }
            continue;
        }
        if (identifier_len == 0U ||
            identifier_len > AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES) {
            return ESP_ERR_INVALID_CRC;
        }
        controller_record_t *record = &table->records[i];
        record->identifier_len = identifier_len;
        memcpy(record->identifier, blob + offset + 1U,
               AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES);
        memcpy(record->public_key,
               blob + offset + 1U + AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES,
               AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
        for (size_t j = identifier_len;
             j < AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES; ++j) {
            if (record->identifier[j] != 0U) return ESP_ERR_INVALID_CRC;
        }
        for (size_t j = 0U; j < i; ++j) {
            if (identifier_equal(&table->records[j], record->identifier,
                                 record->identifier_len)) {
                return ESP_ERR_INVALID_CRC;
            }
        }
    }
    return ESP_OK;
}

static esp_err_t load_table(nvs_handle_t handle, controller_table_t *table)
{
    uint8_t blob[STORE_BLOB_BYTES];
    size_t blob_len = sizeof(blob);
    esp_err_t result = nvs_get_blob(handle, STORE_KEY, blob, &blob_len);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        memset(table, 0, sizeof(*table));
        return ESP_OK;
    }
    if (result != ESP_OK) return result;
    if (blob_len != sizeof(blob)) return ESP_ERR_INVALID_CRC;
    return decode_table(blob, table);
}

static esp_err_t save_table(nvs_handle_t handle, const controller_table_t *table)
{
    uint8_t blob[STORE_BLOB_BYTES];
    encode_table(table, blob);
    esp_err_t result = nvs_set_blob(handle, STORE_KEY, blob, sizeof(blob));
    if (result == ESP_OK) result = nvs_commit(handle);
    memset(blob, 0, sizeof(blob));
    return result;
}

static esp_err_t lock_store(void)
{
    portENTER_CRITICAL(&s_init_mux);
    SemaphoreHandle_t mutex = s_store_mutex;
    portEXIT_CRITICAL(&s_init_mux);
    if (mutex == NULL) return ESP_ERR_INVALID_STATE;
    return xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE
               ? ESP_OK : ESP_FAIL;
}

static void unlock_store(void)
{
    (void)xSemaphoreGive(s_store_mutex);
}

esp_err_t airplay_pairing_store_init(void)
{
    portENTER_CRITICAL(&s_init_mux);
    SemaphoreHandle_t mutex = s_store_mutex;
    portEXIT_CRITICAL(&s_init_mux);
    if (mutex == NULL) {
        SemaphoreHandle_t created = xSemaphoreCreateMutex();
        if (created == NULL) return ESP_ERR_NO_MEM;
        portENTER_CRITICAL(&s_init_mux);
        if (s_store_mutex == NULL) {
            s_store_mutex = created;
            created = NULL;
        }
        mutex = s_store_mutex;
        portEXIT_CRITICAL(&s_init_mux);
        if (created != NULL) vSemaphoreDelete(created);
    }

    if (xSemaphoreTake(mutex, portMAX_DELAY) != pdTRUE) return ESP_FAIL;
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        result = ESP_OK;
    } else if (result == ESP_OK) {
        controller_table_t table;
        result = load_table(handle, &table);
        nvs_close(handle);
        memset(&table, 0, sizeof(table));
    }
    (void)xSemaphoreGive(mutex);
    return result;
}

bool airplay_pairing_store_save_callback(
    const uint8_t *identifier,
    size_t identifier_len,
    const uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context)
{
    (void)context;
    if (!identifier_valid(identifier, identifier_len) || public_key == NULL ||
        lock_store() != ESP_OK) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        controller_table_t table;
        result = load_table(handle, &table);
        size_t index = 0U;
        while (result == ESP_OK && index < table.count &&
               !identifier_equal(&table.records[index], identifier,
                                 identifier_len)) {
            ++index;
        }
        if (result == ESP_OK && index == table.count) {
            if (table.count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS) {
                result = ESP_ERR_NO_MEM;
            } else {
                ++table.count;
            }
        }
        if (result == ESP_OK) {
            controller_record_t *record = &table.records[index];
            memset(record, 0, sizeof(*record));
            record->identifier_len = (uint8_t)identifier_len;
            memcpy(record->identifier, identifier, identifier_len);
            memcpy(record->public_key, public_key,
                   AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
            result = save_table(handle, &table);
        }
        memset(&table, 0, sizeof(table));
        nvs_close(handle);
    }
    unlock_store();
    return result == ESP_OK;
}

bool airplay_pairing_store_lookup_callback(
    const uint8_t *identifier,
    size_t identifier_len,
    uint8_t public_key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
    void *context)
{
    (void)context;
    if (!identifier_valid(identifier, identifier_len) || public_key == NULL ||
        lock_store() != ESP_OK) {
        return false;
    }
    nvs_handle_t handle;
    esp_err_t result = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    bool found = false;
    if (result == ESP_OK) {
        controller_table_t table;
        result = load_table(handle, &table);
        for (size_t i = 0U; result == ESP_OK && i < table.count; ++i) {
            if (identifier_equal(&table.records[i], identifier, identifier_len)) {
                memcpy(public_key, table.records[i].public_key,
                       AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES);
                found = true;
                break;
            }
        }
        memset(&table, 0, sizeof(table));
        nvs_close(handle);
    }
    unlock_store();
    return found;
}

esp_err_t airplay_pairing_store_remove(const uint8_t *identifier,
                                       size_t identifier_len)
{
    if (!identifier_valid(identifier, identifier_len)) return ESP_ERR_INVALID_ARG;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    result = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        controller_table_t table;
        result = load_table(handle, &table);
        size_t index = 0U;
        while (result == ESP_OK && index < table.count &&
               !identifier_equal(&table.records[index], identifier,
                                 identifier_len)) {
            ++index;
        }
        if (result == ESP_OK && index == table.count) {
            result = ESP_ERR_NOT_FOUND;
        } else if (result == ESP_OK) {
            for (size_t i = index; i + 1U < table.count; ++i) {
                table.records[i] = table.records[i + 1U];
            }
            --table.count;
            memset(&table.records[table.count], 0, sizeof(table.records[0]));
            result = save_table(handle, &table);
        }
        memset(&table, 0, sizeof(table));
        nvs_close(handle);
    }
    unlock_store();
    return result;
}

esp_err_t airplay_pairing_store_clear(void)
{
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    result = nvs_open(STORE_NAMESPACE, NVS_READWRITE, &handle);
    if (result == ESP_OK) {
        controller_table_t table = {0};
        result = save_table(handle, &table);
        nvs_close(handle);
    }
    unlock_store();
    return result;
}

esp_err_t airplay_pairing_store_count(size_t *out_count)
{
    if (out_count == NULL) return ESP_ERR_INVALID_ARG;
    *out_count = 0U;
    esp_err_t result = lock_store();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    result = nvs_open(STORE_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        unlock_store();
        return ESP_OK;
    }
    if (result == ESP_OK) {
        controller_table_t table;
        result = load_table(handle, &table);
        if (result == ESP_OK) *out_count = table.count;
        memset(&table, 0, sizeof(table));
        nvs_close(handle);
    }
    unlock_store();
    return result;
}
