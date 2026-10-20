/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"

static inline void *airplay_control_calloc(size_t count, size_t size)
{
    void *memory = heap_caps_calloc(count, size,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return memory != NULL ? memory : calloc(count, size);
}
#else
static inline void *airplay_control_calloc(size_t count, size_t size)
{
    return calloc(count, size);
}
#endif
