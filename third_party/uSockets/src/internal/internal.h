
#ifndef INTERNAL_H
#define INTERNAL_H

#if defined(_MSC_VER)
#define alignas(x) __declspec(align(x))
#else
#include <stdalign.h>
#endif

#include "internal/networking/bsd.h"
#include "internal/socket_context_link.h"

#include <stdint.h>

#ifndef LIBUS_USE_IO_URING

#if defined(LIBUS_USE_EPOLL) || defined(LIBUS_USE_KQUEUE)
#include "internal/eventing/epoll_kqueue.h"
#endif
#ifdef LIBUS_USE_LIBUV
#include "internal/eventing/libuv.h"
#endif
#ifdef LIBUS_USE_GCD
#include "internal/eventing/gcd.h"
#endif
#ifdef LIBUS_USE_ASIO
#include "internal/eventing/asio.h"
#endif

enum {
    POLL_TYPE_SOCKET = 0,
    POLL_TYPE_SOCKET_SHUT_DOWN = 1,
    POLL_TYPE_SEMI_SOCKET = 2,
    POLL_TYPE_CALLBACK = 3,

    POLL_TYPE_POLLING_OUT = 4,
    POLL_TYPE_POLLING_IN = 8
};

void us_internal_dispatch_ready_poll(struct us_poll_t* p, int error, int events);
void us_internal_timer_sweep(struct us_loop_t* loop);
void us_internal_free_closed_sockets(struct us_loop_t* loop);
void us_internal_loop_link(struct us_loop_t* loop, struct us_socket_context_t* context);
void us_internal_loop_unlink(struct us_loop_t* loop, struct us_socket_context_t* context);
void us_internal_loop_data_init(struct us_loop_t* loop, void (*wakeup_cb)(struct us_loop_t* loop),
                                void (*pre_cb)(struct us_loop_t* loop), void (*post_cb)(struct us_loop_t* loop));
void us_internal_loop_data_free(struct us_loop_t* loop);
void us_internal_loop_pre(struct us_loop_t* loop);
void us_internal_loop_post(struct us_loop_t* loop);

struct us_internal_async* us_internal_create_async(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);
void us_internal_async_close(struct us_internal_async* a);
void us_internal_async_set(struct us_internal_async* a, void (*cb)(struct us_internal_async*));
void us_internal_async_wakeup(struct us_internal_async* a);

unsigned int us_internal_accept_poll_event(struct us_poll_t* p);
int us_internal_poll_type(struct us_poll_t* p);
void us_internal_poll_set_type(struct us_poll_t* p, int poll_type);

void us_internal_init_loop_ssl_data(struct us_loop_t* loop);
void us_internal_free_loop_ssl_data(struct us_loop_t* loop);

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

#endif

#ifndef LIBUS_NO_SSL

struct us_internal_ssl_socket_context_t;
struct us_internal_ssl_socket_t;

void us_internal_ssl_socket_context_add_server_name(struct us_internal_ssl_socket_context_t* context,
                                                    const char* hostname_pattern,
                                                    struct us_socket_context_options_t options, void* user);
void us_internal_ssl_socket_context_remove_server_name(struct us_internal_ssl_socket_context_t* context,
                                                       const char* hostname_pattern);
void us_internal_ssl_socket_context_on_server_name(struct us_internal_ssl_socket_context_t* context,
                                                   void (*cb)(struct us_internal_ssl_socket_context_t*, const char*));
void* us_internal_ssl_socket_get_sni_userdata(struct us_internal_ssl_socket_t* s);
void* us_internal_ssl_socket_context_find_server_name_userdata(struct us_internal_ssl_socket_context_t* context,
                                                               const char* hostname_pattern);

void* us_internal_ssl_socket_get_native_handle(struct us_internal_ssl_socket_t* s);
void* us_internal_ssl_socket_context_get_native_handle(struct us_internal_ssl_socket_context_t* context);

struct us_internal_ssl_socket_context_t* us_internal_create_ssl_socket_context(
    struct us_loop_t* loop, int context_ext_size, struct us_socket_context_options_t options);

void us_internal_ssl_socket_context_free(struct us_internal_ssl_socket_context_t* context);
void us_internal_ssl_socket_context_on_open(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_open)(struct us_internal_ssl_socket_t* s, int is_client, char* ip,
                                                int ip_length));

void us_internal_ssl_socket_context_on_close(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_close)(struct us_internal_ssl_socket_t* s, int code, void* reason));

void us_internal_ssl_socket_context_on_data(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_data)(struct us_internal_ssl_socket_t* s, char* data, int length));

void us_internal_ssl_socket_context_on_writable(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_writable)(struct us_internal_ssl_socket_t* s));

void us_internal_ssl_socket_context_on_timeout(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_timeout)(struct us_internal_ssl_socket_t* s));

void us_internal_ssl_socket_context_on_long_timeout(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_timeout)(struct us_internal_ssl_socket_t* s));

void us_internal_ssl_socket_context_on_end(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_end)(struct us_internal_ssl_socket_t* s));

void us_internal_ssl_socket_context_on_connect_error(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_connect_error)(struct us_internal_ssl_socket_t* s, int code));

struct us_listen_socket_t* us_internal_ssl_socket_context_listen(struct us_internal_ssl_socket_context_t* context,
                                                                 const char* host, int port, int options,
                                                                 int socket_ext_size);

struct us_listen_socket_t* us_internal_ssl_socket_context_listen_unix(struct us_internal_ssl_socket_context_t* context,
                                                                      const char* path, int options,
                                                                      int socket_ext_size);

struct us_internal_ssl_socket_t* us_internal_ssl_adopt_accepted_socket(struct us_internal_ssl_socket_context_t* context,
                                                                       LIBUS_SOCKET_DESCRIPTOR accepted_fd,
                                                                       unsigned int socket_ext_size, char* addr_ip,
                                                                       int addr_ip_length);

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_connect(
    struct us_internal_ssl_socket_context_t* context, const char* host, int port, const char* source_host, int options,
    int socket_ext_size);

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_connect_unix(
    struct us_internal_ssl_socket_context_t* context, const char* server_path, int options, int socket_ext_size);

int us_internal_ssl_socket_write(struct us_internal_ssl_socket_t* s, const char* data, int length, int msg_more);
void us_internal_ssl_socket_timeout(struct us_internal_ssl_socket_t* s, unsigned int seconds);
void* us_internal_ssl_socket_context_ext(struct us_internal_ssl_socket_context_t* s);
struct us_internal_ssl_socket_context_t* us_internal_ssl_socket_get_context(struct us_internal_ssl_socket_t* s);
void* us_internal_ssl_socket_ext(struct us_internal_ssl_socket_t* s);
int us_internal_ssl_socket_is_shut_down(struct us_internal_ssl_socket_t* s);
void us_internal_ssl_socket_shutdown(struct us_internal_ssl_socket_t* s);

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_adopt_socket(
    struct us_internal_ssl_socket_context_t* context, struct us_internal_ssl_socket_t* s, int ext_size);

struct us_internal_ssl_socket_context_t* us_internal_create_child_ssl_socket_context(
    struct us_internal_ssl_socket_context_t* context, int context_ext_size);
struct us_loop_t* us_internal_ssl_socket_context_loop(struct us_internal_ssl_socket_context_t* context);

#endif

#endif  // INTERNAL_H
