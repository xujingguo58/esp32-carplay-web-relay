#pragma once

#include <stddef.h>
#include <stdlib.h>

#define MALLOC_CAP_SPIRAM 0x01U
#define MALLOC_CAP_INTERNAL 0x02U
#define MALLOC_CAP_8BIT 0x04U

static inline void *heap_caps_calloc(size_t count, size_t size, unsigned caps)
{
    (void)caps;
    return calloc(count, size);
}

static inline void heap_caps_free(void *memory) { free(memory); }
static inline size_t heap_caps_get_free_size(unsigned caps)
{ (void)caps; return 1024U * 1024U; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps)
{ (void)caps; return 1024U * 1024U; }
