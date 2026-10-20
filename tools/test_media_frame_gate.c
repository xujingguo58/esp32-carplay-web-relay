/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdbool.h>
#include <pthread.h>

#include "media_frame_gate.h"
#include "media_keyframe_gate.h"

typedef struct {
    media_frame_gate_t gate;
    pthread_mutex_t mutex;
    pthread_cond_t changed;
    bool producer_entered;
    bool stop_started;
    bool allow_producer_leave;
    bool drained_after_stop;
} concurrent_gate_context_t;

static void *active_producer(void *argument)
{
    concurrent_gate_context_t *context = argument;
    assert(pthread_mutex_lock(&context->mutex) == 0);
    assert(media_frame_gate_enter(&context->gate));
    context->producer_entered = true;
    assert(pthread_cond_broadcast(&context->changed) == 0);
    while (!context->allow_producer_leave) {
        assert(pthread_cond_wait(&context->changed, &context->mutex) == 0);
    }
    media_frame_gate_leave(&context->gate);
    assert(pthread_cond_broadcast(&context->changed) == 0);
    assert(pthread_mutex_unlock(&context->mutex) == 0);
    return NULL;
}

static void *stop_and_drain(void *argument)
{
    concurrent_gate_context_t *context = argument;
    assert(pthread_mutex_lock(&context->mutex) == 0);
    while (!context->producer_entered) {
        assert(pthread_cond_wait(&context->changed, &context->mutex) == 0);
    }
    media_frame_gate_stop(&context->gate);
    context->stop_started = true;
    assert(pthread_cond_broadcast(&context->changed) == 0);
    while (!media_frame_gate_is_drained(&context->gate)) {
        assert(pthread_cond_wait(&context->changed, &context->mutex) == 0);
    }
    context->drained_after_stop = true;
    assert(pthread_mutex_unlock(&context->mutex) == 0);
    return NULL;
}

static void test_concurrent_stop_drain(void)
{
    concurrent_gate_context_t context = {
        .gate = {.accepting = true, .active_submissions = 0U},
        .mutex = PTHREAD_MUTEX_INITIALIZER,
        .changed = PTHREAD_COND_INITIALIZER,
    };
    pthread_t producer;
    pthread_t stopper;
    assert(pthread_create(&producer, NULL, active_producer, &context) == 0);
    assert(pthread_create(&stopper, NULL, stop_and_drain, &context) == 0);

    assert(pthread_mutex_lock(&context.mutex) == 0);
    while (!context.stop_started) {
        assert(pthread_cond_wait(&context.changed, &context.mutex) == 0);
    }
    assert(!media_frame_gate_enter(&context.gate));
    assert(!media_frame_gate_is_drained(&context.gate));
    context.allow_producer_leave = true;
    assert(pthread_cond_broadcast(&context.changed) == 0);
    assert(pthread_mutex_unlock(&context.mutex) == 0);

    assert(pthread_join(producer, NULL) == 0);
    assert(pthread_join(stopper, NULL) == 0);
    assert(context.drained_after_stop);
    assert(context.gate.active_submissions == 0U);
    assert(!context.gate.accepting);
    assert(pthread_cond_destroy(&context.changed) == 0);
    assert(pthread_mutex_destroy(&context.mutex) == 0);
}

static void test_keyframe_recovery(void)
{
    media_keyframe_gate_t gate = {0};
    const bool non_keyframe = false;
    const bool idr = true;

    media_keyframe_gate_reset(&gate);
    uint32_t generation = media_keyframe_gate_generation(&gate);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    assert(media_keyframe_gate_accepts(&gate, idr));

    /* A failed IDR send leaves the gate closed. */
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_send(&gate, idr, false, generation);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);

    /* A P frame lost after an in-flight IDR invalidates that candidate. */
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_drop(&gate, non_keyframe);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    media_keyframe_gate_note_send(&gate, idr, true, generation);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);

    /* Only a later successfully sent IDR restores dependent frames. */
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_send(&gate, idr, true, generation);
    assert(media_keyframe_gate_accepts(&gate, non_keyframe));

    /* Any loss while streaming re-enters recovery; a failed queued IDR stays closed. */
    media_keyframe_gate_note_drop(&gate, non_keyframe);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_queue_failed(&gate, idr, generation);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_send(&gate, idr, true, generation);
    assert(media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);
    media_keyframe_gate_note_send(&gate, non_keyframe, false, generation);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));

    /* A new SPS/PPS configuration also restarts the decoder reference chain. */
    media_keyframe_gate_reset(&gate);
    assert(!media_keyframe_gate_accepts(&gate, non_keyframe));
    generation = media_keyframe_gate_generation(&gate);
    media_keyframe_gate_note_keyframe_queued(&gate, generation);
    media_keyframe_gate_note_send(&gate, idr, true, generation);
    assert(media_keyframe_gate_accepts(&gate, non_keyframe));
}

int main(void)
{
    media_frame_gate_t gate;
    media_frame_gate_reset(&gate, false);
    assert(media_frame_gate_is_drained(&gate));
    assert(!media_frame_gate_enter(&gate));

    media_frame_gate_reset(&gate, true);
    assert(media_frame_gate_enter(&gate));
    assert(media_frame_gate_enter(&gate));
    assert(gate.active_submissions == 2U);
    assert(!media_frame_gate_is_drained(&gate));

    media_frame_gate_stop(&gate);
    assert(!media_frame_gate_enter(&gate));
    assert(!media_frame_gate_is_drained(&gate));
    media_frame_gate_leave(&gate);
    assert(!media_frame_gate_is_drained(&gate));
    media_frame_gate_leave(&gate);
    assert(media_frame_gate_is_drained(&gate));
    assert(!media_frame_gate_enter(&gate));

    media_frame_gate_reset(&gate, true);
    assert(gate.active_submissions == 0U);
    assert(media_frame_gate_enter(&gate));
    media_frame_gate_leave(&gate);
    assert(media_frame_gate_is_drained(&gate));
    media_frame_gate_leave(&gate);
    assert(gate.active_submissions == 0U);
    test_concurrent_stop_drain();
    test_keyframe_recovery();
    return 0;
}
