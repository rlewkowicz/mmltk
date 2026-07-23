
#ifndef GCD_H
#define GCD_H

#include "internal/loop_data.h"

#include <dispatch/dispatch.h>
#define LIBUS_SOCKET_READABLE 1
#define LIBUS_SOCKET_WRITABLE 2

struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;

    dispatch_queue_t gcd_queue;
};

struct us_poll_t {
    int events;
    dispatch_source_t gcd_read, gcd_write;
    LIBUS_SOCKET_DESCRIPTOR fd;
    unsigned char poll_type;
};

#endif  // GCD_H
