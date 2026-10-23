#pragma once

#include <arpa/inet.h>

#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) \
    (unsigned)(((ip)->addr >> 24U) & 0xffU), \
    (unsigned)(((ip)->addr >> 16U) & 0xffU), \
    (unsigned)(((ip)->addr >> 8U) & 0xffU), \
    (unsigned)((ip)->addr & 0xffU)
