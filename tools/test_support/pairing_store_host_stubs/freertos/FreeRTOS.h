#pragma once

#include <pthread.h>
#include <stdint.h>

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef pthread_mutex_t portMUX_TYPE;
typedef pthread_mutex_t *SemaphoreHandle_t;

#define pdTRUE 1
#define pdFALSE 0
#define portMAX_DELAY UINT32_MAX
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(mux) ((void)pthread_mutex_lock((mux)))
#define portEXIT_CRITICAL(mux) ((void)pthread_mutex_unlock((mux)))

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
