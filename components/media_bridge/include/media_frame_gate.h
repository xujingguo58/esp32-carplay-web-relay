/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The owner must serialize these operations (media_bridge uses s_live_mux). */
typedef struct {
    bool accepting;
    uint32_t active_submissions;
} media_frame_gate_t;

static inline void media_frame_gate_reset(media_frame_gate_t *gate,
                                          bool accepting)
{
    if (gate == NULL) return;
    gate->accepting = accepting;
    gate->active_submissions = 0U;
}

static inline bool media_frame_gate_enter(media_frame_gate_t *gate)
{
    if (gate == NULL || !gate->accepting) return false;
    ++gate->active_submissions;
    return true;
}

static inline void media_frame_gate_leave(media_frame_gate_t *gate)
{
    if (gate != NULL && gate->active_submissions != 0U) {
        --gate->active_submissions;
    }
}

static inline void media_frame_gate_stop(media_frame_gate_t *gate)
{
    if (gate != NULL) gate->accepting = false;
}

static inline bool media_frame_gate_is_drained(const media_frame_gate_t *gate)
{
    return gate == NULL || gate->active_submissions == 0U;
}
