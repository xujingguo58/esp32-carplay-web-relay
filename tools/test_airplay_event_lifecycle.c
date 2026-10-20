/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdbool.h>

#include "airplay_event_lifecycle.h"

int main(void)
{
    assert(airplay_event_lifecycle_may_start(false, false, false));
    assert(airplay_event_lifecycle_may_start(true, true, false));
    assert(!airplay_event_lifecycle_may_start(false, true, false));
    assert(!airplay_event_lifecycle_may_start(false, false, true));
    assert(!airplay_event_lifecycle_may_start(true, true, true));

    assert(!airplay_event_lifecycle_is_unexpected_exit(true, false, false));
    assert(!airplay_event_lifecycle_is_unexpected_exit(false, true, false));
    assert(!airplay_event_lifecycle_is_unexpected_exit(false, false, true));
    assert(airplay_event_lifecycle_is_unexpected_exit(false, false, false));
    return 0;
}
