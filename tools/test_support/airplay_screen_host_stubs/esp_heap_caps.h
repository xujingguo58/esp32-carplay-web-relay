/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM (1U << 0)
#define MALLOC_CAP_8BIT (1U << 1)
#define MALLOC_CAP_INTERNAL (1U << 2)

static inline size_t heap_caps_get_total_size(uint32_t caps)
{
    return (caps & MALLOC_CAP_SPIRAM) != 0U ? (16U * 1024U * 1024U) : 0U;
}

static inline void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    return malloc(size);
}

static inline void *heap_caps_realloc(void *buffer, size_t size, uint32_t caps)
{
    (void)caps;
    return realloc(buffer, size);
}

static inline void heap_caps_free(void *buffer)
{
    free(buffer);
}
