
#ifndef EPOLL_KQUEUE_H
#define EPOLL_KQUEUE_H

#include <stdalign.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

#include "internal/loop_data.h"
#include "libusockets.h"

#define LIBUS_SOCKET_READABLE EPOLLIN
#define LIBUS_SOCKET_WRITABLE EPOLLOUT

struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;

    int num_polls;

    int num_ready_polls;

    int current_ready_poll;

    int fd;

    struct epoll_event ready_polls[1024];
};

struct us_poll_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct {
        signed int fd : 28;
        unsigned int poll_type : 4;
    } state;
    size_t allocation_size;
    int fallthrough;
};

#endif  // EPOLL_KQUEUE_H
