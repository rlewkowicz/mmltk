
#ifndef EPOLL_KQUEUE_H
#define EPOLL_KQUEUE_H

#include "internal/loop_data.h"

#ifdef LIBUS_USE_EPOLL
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#define LIBUS_SOCKET_READABLE EPOLLIN
#define LIBUS_SOCKET_WRITABLE EPOLLOUT
#else
#include <sys/event.h>
#define LIBUS_SOCKET_READABLE 1
#define LIBUS_SOCKET_WRITABLE 2
#endif

struct us_loop_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_internal_loop_data_t data;

    int num_polls;

    int num_ready_polls;

    int current_ready_poll;

    int fd;

#ifdef LIBUS_USE_EPOLL
    struct epoll_event ready_polls[1024];
#else
    struct kevent ready_polls[1024];
#endif
};

struct us_poll_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct {
        signed int fd : 28;
        unsigned int poll_type : 4;
    } state;
};

#endif  // EPOLL_KQUEUE_H
