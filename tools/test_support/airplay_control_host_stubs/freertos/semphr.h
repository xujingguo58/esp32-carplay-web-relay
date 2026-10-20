/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "FreeRTOS.h"

typedef struct test_mutex *SemaphoreHandle_t;

SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
