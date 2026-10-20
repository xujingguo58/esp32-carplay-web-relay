#include "airplay_pairing_store.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "airplay_pair_setup.h"
#include "airplay_pair_verify.h"
#include "nvs.h"

#define MOCK_BLOB_MAX 2048U

static uint8_t s_nvs_blob[MOCK_BLOB_MAX];
static size_t s_nvs_blob_len;
static bool s_nvs_has_blob;
static bool s_fail_commit;
static uint8_t s_pending_blob[MOCK_BLOB_MAX];
static size_t s_pending_blob_len;
static bool s_pending;

typedef struct {
    uint8_t identifier[2];
    uint8_t key_base;
    bool saved;
} save_thread_context_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    pthread_mutex_t *mutex = malloc(sizeof(*mutex));
    if (mutex == NULL) return NULL;
    if (pthread_mutex_init(mutex, NULL) != 0) {
        free(mutex);
        return NULL;
    }
    return mutex;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    (void)timeout;
    return pthread_mutex_lock(semaphore) == 0 ? pdTRUE : pdFALSE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    return pthread_mutex_unlock(semaphore) == 0 ? pdTRUE : pdFALSE;
}

void vSemaphoreDelete(SemaphoreHandle_t semaphore)
{
    if (semaphore == NULL) return;
    (void)pthread_mutex_destroy(semaphore);
    free(semaphore);
}

esp_err_t nvs_open(const char *name, nvs_open_mode_t mode, nvs_handle_t *out_handle)
{
    assert(name != NULL && strcmp(name, "airplay_pair") == 0);
    assert(out_handle != NULL);
    if (mode == NVS_READONLY && !s_nvs_has_blob) return ESP_ERR_NVS_NOT_FOUND;
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    assert(handle == 1);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key,
                       void *out_value, size_t *length)
{
    assert(handle == 1 && key != NULL && strcmp(key, "controllers") == 0);
    assert(length != NULL);
    if (!s_nvs_has_blob) return ESP_ERR_NVS_NOT_FOUND;
    if (*length < s_nvs_blob_len) {
        *length = s_nvs_blob_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, s_nvs_blob, s_nvs_blob_len);
    *length = s_nvs_blob_len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key,
                       const void *value, size_t length)
{
    assert(handle == 1 && key != NULL && strcmp(key, "controllers") == 0);
    assert(value != NULL && length <= sizeof(s_pending_blob));
    memcpy(s_pending_blob, value, length);
    s_pending_blob_len = length;
    s_pending = true;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == 1);
    if (s_fail_commit) {
        s_fail_commit = false;
        s_pending = false;
        return ESP_FAIL;
    }
    assert(s_pending);
    memcpy(s_nvs_blob, s_pending_blob, s_pending_blob_len);
    s_nvs_blob_len = s_pending_blob_len;
    s_nvs_has_blob = true;
    memset(s_pending_blob, 0, sizeof(s_pending_blob));
    s_pending_blob_len = 0U;
    s_pending = false;
    return ESP_OK;
}

void nvs_mock_reset(void)
{
    memset(s_nvs_blob, 0, sizeof(s_nvs_blob));
    memset(s_pending_blob, 0, sizeof(s_pending_blob));
    s_nvs_blob_len = 0U;
    s_pending_blob_len = 0U;
    s_nvs_has_blob = false;
    s_fail_commit = false;
    s_pending = false;
}

void nvs_mock_fail_next_commit(void)
{
    s_fail_commit = true;
}

void nvs_mock_corrupt(size_t offset, unsigned char value)
{
    assert(offset < s_nvs_blob_len);
    s_nvs_blob[offset] = value;
}

void nvs_mock_set_size(size_t size)
{
    assert(size <= sizeof(s_nvs_blob));
    s_nvs_blob_len = size;
    s_nvs_has_blob = true;
}

static void fill_key(uint8_t key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES],
                     uint8_t base)
{
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES; ++i) {
        key[i] = (uint8_t)(base + i);
    }
}

static void *save_from_thread(void *context)
{
    save_thread_context_t *worker = context;
    uint8_t key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES];
    fill_key(key, worker->key_base);
    worker->saved = airplay_pairing_store_save_callback(
        worker->identifier, sizeof(worker->identifier), key, NULL);
    return NULL;
}

int main(void)
{
    airplay_pair_setup_save_pairing_cb_t setup_save =
        airplay_pairing_store_save_callback;
    airplay_pair_verify_lookup_controller_cb_t verify_lookup =
        airplay_pairing_store_lookup_callback;
    assert(setup_save != NULL && verify_lookup != NULL);

    nvs_mock_reset();
    assert(airplay_pairing_store_init() == ESP_OK);
    size_t count = 99U;
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 0U);

    uint8_t key[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES];
    uint8_t recovered[AIRPLAY_PAIRING_STORE_PUBLIC_KEY_BYTES];
    fill_key(key, 3U);
    const uint8_t phone[] = "phone-A";
    assert(!airplay_pairing_store_save_callback(phone, 0U, key, NULL));
    assert(!airplay_pairing_store_save_callback(phone,
               AIRPLAY_PAIRING_STORE_MAX_IDENTIFIER_BYTES + 1U, key, NULL));
    assert(airplay_pairing_store_save_callback(phone, sizeof(phone) - 1U,
                                               key, NULL));
    assert(airplay_pairing_store_lookup_callback(phone, sizeof(phone) - 1U,
                                                 recovered, NULL));
    assert(memcmp(key, recovered, sizeof(key)) == 0);
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 1U);

    fill_key(key, 91U);
    assert(airplay_pairing_store_save_callback(phone, sizeof(phone) - 1U,
                                               key, NULL));
    assert(airplay_pairing_store_lookup_callback(phone, sizeof(phone) - 1U,
                                                 recovered, NULL));
    assert(memcmp(key, recovered, sizeof(key)) == 0);
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 1U);

    uint8_t identifiers[AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS][2];
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS - 1U; ++i) {
        identifiers[i][0] = 'C';
        identifiers[i][1] = (uint8_t)('0' + i);
        fill_key(key, (uint8_t)(i + 1U));
        assert(airplay_pairing_store_save_callback(identifiers[i], 2U,
                                                   key, NULL));
    }
    assert(airplay_pairing_store_count(&count) == ESP_OK &&
           count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS);
    const uint8_t overflow[] = {'O', 'V'};
    assert(!airplay_pairing_store_save_callback(overflow, sizeof(overflow),
                                               key, NULL));
    assert(airplay_pairing_store_remove(identifiers[3], 2U) == ESP_OK);
    assert(!airplay_pairing_store_lookup_callback(identifiers[3], 2U,
                                                  recovered, NULL));
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 7U);
    assert(airplay_pairing_store_remove(identifiers[3], 2U) == ESP_ERR_NOT_FOUND);

    assert(airplay_pairing_store_clear() == ESP_OK);
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 0U);

    pthread_t threads[AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS];
    save_thread_context_t workers[AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS] = {0};
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS; ++i) {
        workers[i].identifier[0] = 'T';
        workers[i].identifier[1] = (uint8_t)('0' + i);
        workers[i].key_base = (uint8_t)(i * 7U);
        assert(pthread_create(&threads[i], NULL, save_from_thread,
                              &workers[i]) == 0);
    }
    for (size_t i = 0U; i < AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(workers[i].saved);
    }
    assert(airplay_pairing_store_count(&count) == ESP_OK &&
           count == AIRPLAY_PAIRING_STORE_MAX_CONTROLLERS);
    assert(airplay_pairing_store_clear() == ESP_OK);

    nvs_mock_fail_next_commit();
    assert(!airplay_pairing_store_save_callback(phone, sizeof(phone) - 1U,
                                                key, NULL));
    assert(!airplay_pairing_store_lookup_callback(phone, sizeof(phone) - 1U,
                                                  recovered, NULL));

    assert(airplay_pairing_store_save_callback(phone, sizeof(phone) - 1U,
                                               key, NULL));
    nvs_mock_corrupt(0U, (unsigned char)'X');
    assert(!airplay_pairing_store_lookup_callback(phone, sizeof(phone) - 1U,
                                                  recovered, NULL));
    assert(airplay_pairing_store_clear() == ESP_OK);
    assert(airplay_pairing_store_count(&count) == ESP_OK && count == 0U);
    assert(airplay_pairing_store_save_callback(phone, sizeof(phone) - 1U,
                                               key, NULL));
    nvs_mock_set_size(5U);
    assert(!airplay_pairing_store_lookup_callback(phone, sizeof(phone) - 1U,
                                                  recovered, NULL));

    puts("airplay pairing store host test passed");
    return 0;
}
