/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    uint8_t *data;
    size_t size;
} media_h264_prepared_frame_t;

/* Prepare an Annex-B access unit in its receive buffer. On keyframes, the
 * SPS/PPS bytes are copied into the already reserved prefix; frame bytes stay
 * in place. */
static inline bool media_h264_prepare_borrowed_frame(
    uint8_t *access_unit,
    size_t prefix_capacity,
    size_t access_unit_size,
    const uint8_t *parameter_sets,
    size_t parameter_sets_size,
    bool prepend_parameter_sets,
    media_h264_prepared_frame_t *prepared)
{
    if (access_unit == NULL || access_unit_size == 0U || prepared == NULL ||
        (parameter_sets == NULL && parameter_sets_size != 0U)) {
        return false;
    }
    size_t prefix_size = prepend_parameter_sets ? parameter_sets_size : 0U;
    if (prefix_size > prefix_capacity ||
        access_unit_size > SIZE_MAX - prefix_size) {
        return false;
    }
    if (prefix_size != 0U) {
        memcpy(access_unit - prefix_size, parameter_sets, prefix_size);
    }
    prepared->data = access_unit - prefix_size;
    prepared->size = prefix_size + access_unit_size;
    return true;
}
