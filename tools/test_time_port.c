#include "time_port.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static int64_t s_fake_time_us;

int64_t esp_timer_get_time(void)
{
    return s_fake_time_us;
}

int main(void)
{
    s_fake_time_us = 0;
    assert(time_port_monotonic_us() == 0);
    assert(time_port_monotonic_ms() == 0);

    s_fake_time_us = 1234567;
    assert(time_port_monotonic_us() == 1234567);
    assert(time_port_monotonic_ms() == 1234);

    s_fake_time_us = 9876543210LL;
    assert(time_port_monotonic_us() == 9876543210LL);
    assert(time_port_monotonic_ms() == 9876543);

    puts("time_port host test passed");
    return 0;
}
