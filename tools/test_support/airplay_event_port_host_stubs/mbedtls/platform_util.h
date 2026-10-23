#pragma once

#include <stddef.h>
#include <string.h>

static inline void mbedtls_platform_zeroize(void *buffer, size_t length)
{
    volatile unsigned char *bytes = buffer;
    while (length-- != 0U) *bytes++ = 0U;
}
