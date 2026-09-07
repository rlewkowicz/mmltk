
#ifndef INTERNAL_H
#define INTERNAL_H

#include <stdalign.h>
#include <stdint.h>

#include "internal/eventing/epoll_kqueue.h"
#include "internal/networking/bsd.h"
#include "internal/socket_context_link.h"

enum {
    POLL_TYPE_SOCKET = 0,
    POLL_TYPE_SOCKET_SHUT_DOWN = 1,
    POLL_TYPE_SEMI_SOCKET = 2,
    POLL_TYPE_CALLBACK = 3,

    POLL_TYPE_POLLING_OUT = 4,
    POLL_TYPE_POLLING_IN = 8
};

enum us_internal_callback_kind {
    CALLBACK_KIND_TIMER_OR_UDP = 0,
    CALLBACK_KIND_ASYNC = 1,
    CALLBACK_KIND_FD_WATCHER = 2
};

void us_internal_dispatch_ready_poll(struct us_poll_t* p, int error, int events, unsigned int raw_events);
void us_internal_timer_sweep(struct us_loop_t* loop);
void us_internal_free_closed_sockets(struct us_loop_t* loop);
void us_internal_loop_link(struct us_loop_t* loop, struct us_socket_context_t* context);
void us_internal_loop_unlink(struct us_loop_t* loop, struct us_socket_context_t* context);
int us_internal_loop_data_init(struct us_loop_t* loop, void (*wakeup_cb)(struct us_loop_t* loop),
                               void (*pre_cb)(struct us_loop_t* loop), void (*post_cb)(struct us_loop_t* loop));
void us_internal_loop_data_free(struct us_loop_t* loop);
void us_internal_loop_pre(struct us_loop_t* loop);
void us_internal_loop_post(struct us_loop_t* loop);

struct us_internal_async* us_internal_create_async(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);
void us_internal_async_close(struct us_internal_async* a);
int us_internal_async_set(struct us_internal_async* a, void (*cb)(struct us_internal_async*));
void us_internal_async_wakeup(struct us_internal_async* a);

unsigned int us_internal_accept_poll_event(struct us_poll_t* p);
int us_internal_poll_type(struct us_poll_t* p);
void us_internal_poll_set_type(struct us_poll_t* p, int poll_type);
struct us_poll_t* us_internal_adopt_poll(struct us_loop_t* loop, LIBUS_SOCKET_DESCRIPTOR fd, unsigned int poll_ext_size,
                                         int poll_type, int events);

void us_internal_socket_context_link_socket(struct us_socket_context_t* context, struct us_socket_t* s);
void us_internal_socket_context_unlink_socket(struct us_socket_context_t* context, struct us_socket_t* s);

struct us_socket_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_poll_t p;
    unsigned char timeout;
    unsigned char long_timeout;
    unsigned short low_prio_state;
    struct us_socket_context_t* context;
    struct us_socket_t *prev, *next;
};

struct us_internal_callback_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_poll_t p;
    struct us_loop_t* loop;
    int cb_expects_the_loop;
    int leave_poll_ready;
    void (*cb)(struct us_internal_callback_t* cb);
};

struct us_internal_fd_watcher_t {
    struct us_internal_callback_t cb;
    void (*ready_cb)(struct us_fd_watcher_t* watcher, unsigned int events);
};

struct us_listen_socket_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_socket_t s;
    unsigned int socket_ext_size;
};

void us_internal_socket_context_link_listen_socket(struct us_socket_context_t* context, struct us_listen_socket_t* s);
void us_internal_socket_context_unlink_listen_socket(struct us_socket_context_t* context, struct us_listen_socket_t* s);

struct us_socket_context_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_loop_t* loop;
    LIBUS_SOCKET_CONTEXT_SHARED_FIELDS

    LIBUS_SOCKET_DESCRIPTOR(*on_pre_open)
    (struct us_socket_context_t* context, LIBUS_SOCKET_DESCRIPTOR fd, char* ip, int ip_length);
    LIBUS_SOCKET_CONTEXT_CALLBACK_FIELDS
    int (*is_low_prio)(struct us_socket_t*);
};

#endif  // INTERNAL_H
