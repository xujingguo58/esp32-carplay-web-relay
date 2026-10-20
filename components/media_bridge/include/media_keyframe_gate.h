/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Keep the decoder reference chain valid after startup, reconfiguration, or loss. */
typedef struct {
    bool waiting_for_keyframe;
    bool keyframe_pending;
    uint32_t generation;
} media_keyframe_gate_t;

static inline void media_keyframe_gate_reset(media_keyframe_gate_t *gate)
{
    if (gate == NULL) return;
    ++gate->generation;
    gate->waiting_for_keyframe = true;
    gate->keyframe_pending = false;
}

static inline bool media_keyframe_gate_accepts(const media_keyframe_gate_t *gate,
                                               bool is_keyframe)
{
    return gate != NULL && (!gate->waiting_for_keyframe || is_keyframe);
}

static inline uint32_t media_keyframe_gate_generation(const media_keyframe_gate_t *gate)
{
    return gate != NULL ? gate->generation : 0U;
}

static inline void media_keyframe_gate_note_drop(media_keyframe_gate_t *gate,
                                                 bool is_keyframe)
{
    if (gate == NULL) return;
    if (!gate->waiting_for_keyframe) {
        ++gate->generation;
        gate->waiting_for_keyframe = true;
        gate->keyframe_pending = false;
    } else if (gate->keyframe_pending && !is_keyframe) {
        /* A dependent frame after a queued IDR was lost; that IDR cannot
         * restore the reference chain for later pictures. */
        ++gate->generation;
        gate->keyframe_pending = false;
    }
}

static inline void media_keyframe_gate_note_keyframe_queued(
    media_keyframe_gate_t *gate, uint32_t generation)
{
    if (gate != NULL && gate->waiting_for_keyframe &&
        gate->generation == generation) {
        gate->keyframe_pending = true;
    }
}

static inline void media_keyframe_gate_note_queue_failed(
    media_keyframe_gate_t *gate, bool is_keyframe, uint32_t generation)
{
    if (gate == NULL) return;
    if (is_keyframe && gate->waiting_for_keyframe &&
        gate->keyframe_pending && gate->generation == generation) {
        ++gate->generation;
        gate->keyframe_pending = false;
        return;
    }
    media_keyframe_gate_note_drop(gate, is_keyframe);
}

static inline void media_keyframe_gate_note_send(media_keyframe_gate_t *gate,
                                                 bool is_keyframe,
                                                 bool sent,
                                                 uint32_t generation)
{
    if (gate == NULL) return;
    if (!sent) {
        media_keyframe_gate_note_queue_failed(gate, is_keyframe, generation);
    } else if (is_keyframe && gate->waiting_for_keyframe &&
               gate->keyframe_pending && gate->generation == generation) {
        gate->waiting_for_keyframe = false;
        gate->keyframe_pending = false;
    }
}
