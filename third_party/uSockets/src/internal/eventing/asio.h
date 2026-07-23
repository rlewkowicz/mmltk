
#ifndef ASIO_H
#define ASIO_H

#include "internal/loop_data.h"

#define LIBUS_SOCKET_READABLE 1
#define LIBUS_SOCKET_WRITABLE 2

struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;

    void* io;

    int is_default;
};

struct us_poll_t {
    void* boost_block;

    LIBUS_SOCKET_DESCRIPTOR fd;
    unsigned char poll_type;
    int events;
};

#endif  // ASIO_H
