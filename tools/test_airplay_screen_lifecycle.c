/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>

#include "airplay_screen_lifecycle.h"

int main(void)
{
    assert(airplay_screen_lifecycle_may_accept(false, false));
    assert(!airplay_screen_lifecycle_may_accept(true, false));
    assert(!airplay_screen_lifecycle_may_accept(false, true));

    assert(airplay_screen_lifecycle_is_unexpected_exit(false));
    assert(!airplay_screen_lifecycle_is_unexpected_exit(true));
    return 0;
}
