
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "internal/internal.h"
#include "libusockets.h"

int default_is_low_prio_handler(struct us_socket_t* s) {
    return 0;
}

unsigned short us_socket_context_timestamp(int ssl, struct us_socket_context_t* context) {
    return context->timestamp;
}

void us_listen_socket_close(int ssl, struct us_listen_socket_t* ls) {
    if (!us_socket_is_closed(0, &ls->s)) {
        us_internal_socket_context_unlink_listen_socket(ls->s.context, ls);
        us_poll_stop((struct us_poll_t*)&ls->s, ls->s.context->loop);
        bsd_close_socket(us_poll_fd((struct us_poll_t*)&ls->s));

        ls->s.next = ls->s.context->loop->data.closed_head;
        ls->s.context->loop->data.closed_head = &ls->s;

        ls->s.prev = (struct us_socket_t*)ls->s.context;
    }
}

void us_socket_context_close(int ssl, struct us_socket_context_t* context) {
    struct us_listen_socket_t* ls = context->head_listen_sockets;
    while (ls) {
        struct us_listen_socket_t* nextLS = (struct us_listen_socket_t*)ls->s.next;
        us_listen_socket_close(ssl, ls);
        ls = nextLS;
    }

    struct us_socket_t* s = context->head_sockets;
    while (s) {
        struct us_socket_t* nextS = s->next;
        us_socket_close(ssl, s, 0, 0);
        s = nextS;
    }
}

void us_internal_socket_context_unlink_listen_socket(struct us_socket_context_t* context,
                                                     struct us_listen_socket_t* ls) {
    if (ls == (struct us_listen_socket_t*)context->iterator) {
        context->iterator = ls->s.next;
    }

    if (ls->s.prev == ls->s.next) {
        context->head_listen_sockets = 0;
    } else {
        if (ls->s.prev) {
            ls->s.prev->next = ls->s.next;
        } else {
            context->head_listen_sockets = (struct us_listen_socket_t*)ls->s.next;
        }
        if (ls->s.next) {
            ls->s.next->prev = ls->s.prev;
        }
    }
}

void us_internal_socket_context_unlink_socket(struct us_socket_context_t* context, struct us_socket_t* s) {
    if (s == context->iterator) {
        context->iterator = s->next;
    }

    if (s->prev == s->next) {
        context->head_sockets = 0;
    } else {
        if (s->prev) {
            s->prev->next = s->next;
        } else {
            context->head_sockets = s->next;
        }
        if (s->next) {
            s->next->prev = s->prev;
        }
    }
}

void us_internal_socket_context_link_listen_socket(struct us_socket_context_t* context, struct us_listen_socket_t* ls) {
    ls->s.context = context;
    ls->s.next = (struct us_socket_t*)context->head_listen_sockets;
    ls->s.prev = 0;
    if (context->head_listen_sockets) {
        context->head_listen_sockets->s.prev = &ls->s;
    }
    context->head_listen_sockets = ls;
}

void us_internal_socket_context_link_socket(struct us_socket_context_t* context, struct us_socket_t* s) {
    us_internal_link_socket_to_context(context, s);
}

static void us_internal_socket_reset_state(struct us_socket_t* s, struct us_socket_context_t* context) {
    s->context = context;
    s->timeout = 255;
    s->long_timeout = 255;
    s->low_prio_state = 0;
}

static struct us_poll_t* us_internal_socket_context_adopt_poll(struct us_socket_context_t* context,
                                                               LIBUS_SOCKET_DESCRIPTOR fd, int socket_ext_size,
                                                               unsigned int poll_ext_size, int events) {
    if (fd == LIBUS_SOCKET_ERROR) {
        return 0;
    }
    if (!context || socket_ext_size < 0) {
        bsd_close_socket(fd);
        errno = EINVAL;
        return NULL;
    }

    return us_internal_adopt_poll(context->loop, fd, poll_ext_size, POLL_TYPE_SEMI_SOCKET, events);
}

/* Takes ownership of a freshly created listen fd, or returns 0 if creating it failed. */
static struct us_listen_socket_t* us_internal_socket_context_adopt_listen_socket(struct us_socket_context_t* context,
                                                                                 LIBUS_SOCKET_DESCRIPTOR fd,
                                                                                 int socket_ext_size) {
    struct us_poll_t* p = us_internal_socket_context_adopt_poll(
        context, fd, socket_ext_size, sizeof(struct us_listen_socket_t) - sizeof(struct us_poll_t),
        LIBUS_SOCKET_READABLE);
    if (!p) {
        return NULL;
    }
    struct us_listen_socket_t* ls = (struct us_listen_socket_t*)p;
    us_internal_socket_reset_state(&ls->s, context);
    ls->s.next = 0;
    us_internal_socket_context_link_listen_socket(context, ls);
    ls->socket_ext_size = socket_ext_size;
    return ls;
}

/* Takes ownership of a freshly created connect fd, or returns 0 if creating it failed. */
static struct us_socket_t* us_internal_socket_context_adopt_connect_socket(struct us_socket_context_t* context,
                                                                           LIBUS_SOCKET_DESCRIPTOR fd,
                                                                           int socket_ext_size) {
    struct us_poll_t* p =
        us_internal_socket_context_adopt_poll(context, fd, socket_ext_size,
                                              sizeof(struct us_socket_t) - sizeof(struct us_poll_t) +
                                                  (unsigned int)(socket_ext_size < 0 ? 0 : socket_ext_size),
                                              LIBUS_SOCKET_WRITABLE);
    if (!p) {
        return NULL;
    }
    struct us_socket_t* connect_socket = (struct us_socket_t*)p;
    us_internal_socket_reset_state(connect_socket, context);
    us_internal_socket_context_link_socket(context, connect_socket);
    return connect_socket;
}

struct us_loop_t* us_socket_context_loop(int ssl, struct us_socket_context_t* context) {
    return context->loop;
}

void* us_socket_context_find_server_name_userdata(int ssl, struct us_socket_context_t* context,
                                                  const char* hostname_pattern) {
    return NULL;
}

void* us_socket_server_name_userdata(int ssl, struct us_socket_t* s) {
    return NULL;
}

void us_socket_context_add_server_name(int ssl, struct us_socket_context_t* context, const char* hostname_pattern,
                                       struct us_socket_context_options_t options, void* user) {}

void us_socket_context_remove_server_name(int ssl, struct us_socket_context_t* context, const char* hostname_pattern) {}

void us_socket_context_on_server_name(int ssl, struct us_socket_context_t* context,
                                      void (*cb)(struct us_socket_context_t*, const char* hostname)) {}

void* us_socket_context_get_native_handle(int ssl, struct us_socket_context_t* context) {
    return 0;
}

struct us_socket_context_t* us_create_socket_context(int ssl, struct us_loop_t* loop, int context_ext_size,
                                                     struct us_socket_context_options_t options) {
    if (!loop || context_ext_size < 0) {
        errno = EINVAL;
        return NULL;
    }
    if ((size_t)context_ext_size > SIZE_MAX - sizeof(struct us_socket_context_t)) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct us_socket_context_t* context = malloc(sizeof(struct us_socket_context_t) + context_ext_size);
    if (!context) {
        errno = ENOMEM;
        return NULL;
    }
    context->loop = loop;
    context->head_sockets = 0;
    context->head_listen_sockets = 0;
    context->iterator = 0;
    context->next = 0;
    context->is_low_prio = default_is_low_prio_handler;

    context->timestamp = 0;
    context->long_timestamp = 0;
    context->global_tick = 0;

    context->on_pre_open = 0;

    us_internal_loop_link(loop, context);

    return context;
}

void us_socket_context_free(int ssl, struct us_socket_context_t* context) {
    us_internal_loop_unlink(context->loop, context);
    free(context);
}

struct us_listen_socket_t* us_socket_context_listen(int ssl, struct us_socket_context_t* context, const char* host,
                                                    int port, int options, int socket_ext_size) {
    return us_internal_socket_context_adopt_listen_socket(context, bsd_create_listen_socket(host, port, options),
                                                          socket_ext_size);
}

struct us_listen_socket_t* us_socket_context_listen_unix(int ssl, struct us_socket_context_t* context, const char* path,
                                                         int options, int socket_ext_size) {
    return us_internal_socket_context_adopt_listen_socket(context, bsd_create_listen_socket_unix(path, options),
                                                          socket_ext_size);
}

struct us_socket_t* us_socket_context_connect(int ssl, struct us_socket_context_t* context, const char* host, int port,
                                              const char* source_host, int options, int socket_ext_size) {
    return us_internal_socket_context_adopt_connect_socket(
        context, bsd_create_connect_socket(host, port, source_host, options), socket_ext_size);
}

struct us_socket_t* us_socket_context_connect_unix(int ssl, struct us_socket_context_t* context,
                                                   const char* server_path, int options, int socket_ext_size) {
    return us_internal_socket_context_adopt_connect_socket(
        context, bsd_create_connect_socket_unix(server_path, options), socket_ext_size);
}

struct us_socket_context_t* us_create_child_socket_context(int ssl, struct us_socket_context_t* context,
                                                           int context_ext_size) {
    if (!context) {
        errno = EINVAL;
        return NULL;
    }
    struct us_socket_context_options_t options = {0};
    return us_create_socket_context(ssl, context->loop, context_ext_size, options);
}

struct us_socket_t* us_socket_context_adopt_socket(int ssl, struct us_socket_context_t* context, struct us_socket_t* s,
                                                   int ext_size) {
    if (!context || !s || ext_size < 0) {
        errno = EINVAL;
        return NULL;
    }
    if (us_socket_is_closed(ssl, s)) {
        return s;
    }

    struct us_socket_context_t* source_context = s->context;
    const int was_low_priority = s->low_prio_state == 1;
    if (!was_low_priority) {
        us_internal_socket_context_unlink_socket(source_context, s);
    }

    struct us_socket_t* new_s =
        (struct us_socket_t*)us_poll_resize(&s->p, source_context->loop, sizeof(struct us_socket_t) + ext_size);
    if (!new_s) {
        if (!was_low_priority) {
            us_internal_socket_context_link_socket(source_context, s);
        }
        return NULL;
    }
    new_s->timeout = 255;
    new_s->long_timeout = 255;

    if (new_s->low_prio_state == 1) {
        new_s->context = context;
        if (!new_s->prev)
            context->loop->data.low_prio_head = new_s;
        else
            new_s->prev->next = new_s;

        if (new_s->next)
            new_s->next->prev = new_s;
    } else {
        us_internal_socket_context_link_socket(context, new_s);
    }

    return new_s;
}

void us_socket_context_on_pre_open(int ssl, struct us_socket_context_t* context,
                                   LIBUS_SOCKET_DESCRIPTOR (*on_pre_open)(struct us_socket_context_t* context,
                                                                          LIBUS_SOCKET_DESCRIPTOR fd, char* ip,
                                                                          int ip_length)) {
    context->on_pre_open = on_pre_open;
}

void us_socket_context_on_open(int ssl, struct us_socket_context_t* context,
                               struct us_socket_t* (*on_open)(struct us_socket_t* s, int is_client, char* ip,
                                                              int ip_length)) {
    context->on_open = on_open;
}

void us_socket_context_on_close(int ssl, struct us_socket_context_t* context,
                                struct us_socket_t* (*on_close)(struct us_socket_t* s, int code, void* reason)) {
    context->on_close = on_close;
}

void us_socket_context_on_data(int ssl, struct us_socket_context_t* context,
                               struct us_socket_t* (*on_data)(struct us_socket_t* s, char* data, int length)) {
    context->on_data = on_data;
}

void us_socket_context_on_writable(int ssl, struct us_socket_context_t* context,
                                   struct us_socket_t* (*on_writable)(struct us_socket_t* s)) {
    context->on_writable = on_writable;
}

void us_socket_context_on_long_timeout(int ssl, struct us_socket_context_t* context,
                                       struct us_socket_t* (*on_long_timeout)(struct us_socket_t*)) {
    context->on_socket_long_timeout = on_long_timeout;
}

void us_socket_context_on_timeout(int ssl, struct us_socket_context_t* context,
                                  struct us_socket_t* (*on_timeout)(struct us_socket_t*)) {
    context->on_socket_timeout = on_timeout;
}

void us_socket_context_on_end(int ssl, struct us_socket_context_t* context,
                              struct us_socket_t* (*on_end)(struct us_socket_t*)) {
    context->on_end = on_end;
}

void us_socket_context_on_connect_error(int ssl, struct us_socket_context_t* context,
                                        struct us_socket_t* (*on_connect_error)(struct us_socket_t* s, int code)) {
    context->on_connect_error = on_connect_error;
}

void* us_socket_context_ext(int ssl, struct us_socket_context_t* context) {
    return context + 1;
}
