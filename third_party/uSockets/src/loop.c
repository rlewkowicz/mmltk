/*
 * Authored by Alex Hultman, 2018-2021.
 * Intellectual property of third-party.

 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at

 *     http://www.apache.org/licenses/LICENSE-2.0

 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef LIBUS_USE_IO_URING

#include "libusockets.h"
#include "internal/internal.h"
#include "internal/timer_sweep.h"
#include <stdlib.h>

void us_internal_loop_data_init(struct us_loop_t* loop, void (*wakeup_cb)(struct us_loop_t* loop),
                                void (*pre_cb)(struct us_loop_t* loop), void (*post_cb)(struct us_loop_t* loop)) {
    loop->data.sweep_timer = us_create_timer(loop, 1, 0);
    loop->data.recv_buf = malloc(LIBUS_RECV_BUFFER_LENGTH + LIBUS_RECV_BUFFER_PADDING * 2);
    loop->data.ssl_data = 0;
    loop->data.head = 0;
    loop->data.iterator = 0;
    loop->data.closed_head = 0;
    loop->data.low_prio_head = 0;
    loop->data.low_prio_budget = 0;

    loop->data.pre_cb = pre_cb;
    loop->data.post_cb = post_cb;
    loop->data.iteration_nr = 0;

    loop->data.wakeup_async = us_internal_create_async(loop, 1, 0);
    us_internal_async_set(loop->data.wakeup_async, (void (*)(struct us_internal_async*))wakeup_cb);
}

void us_internal_loop_data_free(struct us_loop_t* loop) {
#ifndef LIBUS_NO_SSL
    us_internal_free_loop_ssl_data(loop);
#endif

    free(loop->data.recv_buf);

    us_timer_close(loop->data.sweep_timer);
    us_internal_async_close(loop->data.wakeup_async);
}

void us_wakeup_loop(struct us_loop_t* loop) {
    us_internal_async_wakeup(loop->data.wakeup_async);
}

void us_internal_loop_link(struct us_loop_t* loop, struct us_socket_context_t* context) {
    context->next = loop->data.head;
    context->prev = 0;
    if (loop->data.head) {
        loop->data.head->prev = context;
    }
    loop->data.head = context;
}

void us_internal_loop_unlink(struct us_loop_t* loop, struct us_socket_context_t* context) {
    if (loop->data.head == context) {
        loop->data.head = context->next;
        if (loop->data.head) {
            loop->data.head->prev = 0;
        }
    } else {
        context->prev->next = context->next;
        if (context->next) {
            context->next->prev = context->prev;
        }
    }
}

void us_internal_timer_sweep(struct us_loop_t* loop) {
    struct us_internal_loop_data_t* loop_data = &loop->data;
    LIBUS_INTERNAL_TIMER_SWEEP(loop_data);
}

static const int MAX_LOW_PRIO_SOCKETS_PER_LOOP_ITERATION = 5;

void us_internal_handle_low_priority_sockets(struct us_loop_t* loop) {
    struct us_internal_loop_data_t* loop_data = &loop->data;
    struct us_socket_t* s;

    loop_data->low_prio_budget = MAX_LOW_PRIO_SOCKETS_PER_LOOP_ITERATION;

    for (s = loop_data->low_prio_head; s && loop_data->low_prio_budget > 0;
         s = loop_data->low_prio_head, loop_data->low_prio_budget--) {
        loop_data->low_prio_head = s->next;
        if (s->next)
            s->next->prev = 0;
        s->next = 0;

        us_internal_socket_context_link_socket(s->context, s);
        us_poll_change(&s->p, us_socket_context(0, s)->loop, us_poll_events(&s->p) | LIBUS_SOCKET_READABLE);

        s->low_prio_state = 2;
    }
}

void us_internal_free_closed_sockets(struct us_loop_t* loop) {
    if (loop->data.closed_head) {
        for (struct us_socket_t* s = loop->data.closed_head; s;) {
            struct us_socket_t* next = s->next;
            us_poll_free((struct us_poll_t*)s, loop);
            s = next;
        }
        loop->data.closed_head = 0;
    }
}

void sweep_timer_cb(struct us_internal_callback_t* cb) {
    us_internal_timer_sweep(cb->loop);
}

long long us_loop_iteration_number(struct us_loop_t* loop) {
    return loop->data.iteration_nr;
}

void us_internal_loop_pre(struct us_loop_t* loop) {
    loop->data.iteration_nr++;
    us_internal_handle_low_priority_sockets(loop);
    loop->data.pre_cb(loop);
}

void us_internal_loop_post(struct us_loop_t* loop) {
    us_internal_free_closed_sockets(loop);
    loop->data.post_cb(loop);
}

struct us_socket_t* us_adopt_accepted_socket(int ssl, struct us_socket_context_t* context,
                                             LIBUS_SOCKET_DESCRIPTOR accepted_fd, unsigned int socket_ext_size,
                                             char* addr_ip, int addr_ip_length) {
#ifndef LIBUS_NO_SSL
    if (ssl) {
        return (struct us_socket_t*)us_internal_ssl_adopt_accepted_socket(
            (struct us_internal_ssl_socket_context_t*)context, accepted_fd, socket_ext_size, addr_ip, addr_ip_length);
    }
#endif
    struct us_poll_t* accepted_p =
        us_create_poll(context->loop, 0, sizeof(struct us_socket_t) - sizeof(struct us_poll_t) + socket_ext_size);
    us_poll_init(accepted_p, accepted_fd, POLL_TYPE_SOCKET);
    us_poll_start(accepted_p, context->loop, LIBUS_SOCKET_READABLE);

    struct us_socket_t* s = (struct us_socket_t*)accepted_p;

    s->context = context;
    s->timeout = 255;
    s->long_timeout = 255;
    s->low_prio_state = 0;

    bsd_socket_nodelay(accepted_fd, 1);

    us_internal_socket_context_link_socket(context, s);

    context->on_open(s, 0, addr_ip, addr_ip_length);
    return s;
}

void us_internal_dispatch_ready_poll(struct us_poll_t* p, int error, int events) {
    switch (us_internal_poll_type(p)) {
        case POLL_TYPE_CALLBACK: {
            struct us_internal_callback_t* cb = (struct us_internal_callback_t*)p;
            if (!cb->leave_poll_ready) {
#ifndef LIBUS_USE_LIBUV
                us_internal_accept_poll_event(p);
#endif
            }
            cb->cb(cb->cb_expects_the_loop ? (struct us_internal_callback_t*)cb->loop
                                           : (struct us_internal_callback_t*)&cb->p);
        } break;
        case POLL_TYPE_SEMI_SOCKET: {
            if (us_poll_events(p) == LIBUS_SOCKET_WRITABLE) {
                struct us_socket_t* s = (struct us_socket_t*)p;

                if (error) {
                    s->context->on_connect_error(s, 0);
                    us_socket_close_connecting(0, s);
                } else {
                    us_poll_change(p, s->context->loop, LIBUS_SOCKET_READABLE);

                    bsd_socket_nodelay(us_poll_fd(p), 1);

                    us_internal_poll_set_type(p, POLL_TYPE_SOCKET);

                    us_socket_timeout(0, s, 0);

                    s->context->on_open(s, 1, 0, 0);
                }
            } else {
                struct us_listen_socket_t* listen_socket = (struct us_listen_socket_t*)p;
                struct bsd_addr_t addr;

                LIBUS_SOCKET_DESCRIPTOR client_fd = bsd_accept_socket(us_poll_fd(p), &addr);
                if (client_fd == LIBUS_SOCKET_ERROR) {
                } else {
                    do {
                        struct us_socket_context_t* context = us_socket_context(0, &listen_socket->s);
                        if (context->on_pre_open == 0 ||
                            context->on_pre_open(context, client_fd, bsd_addr_get_ip(&addr),
                                                 bsd_addr_get_ip_length(&addr)) == client_fd) {
                            us_adopt_accepted_socket(0, context, client_fd, listen_socket->socket_ext_size,
                                                     bsd_addr_get_ip(&addr), bsd_addr_get_ip_length(&addr));

                            if (us_socket_is_closed(0, &listen_socket->s)) {
                                break;
                            }
                        }

                    } while ((client_fd = bsd_accept_socket(us_poll_fd(p), &addr)) != LIBUS_SOCKET_ERROR);
                }
            }
        } break;
        case POLL_TYPE_SOCKET_SHUT_DOWN:
        case POLL_TYPE_SOCKET: {
            struct us_socket_t* s = (struct us_socket_t*)p;

            if (error) {
                s = us_socket_close(0, s, 0, NULL);
                return;
            }

            if (events & LIBUS_SOCKET_WRITABLE) {
                s->context->loop->data.last_write_failed = 0;

                s = s->context->on_writable(s);

                if (us_socket_is_closed(0, s)) {
                    return;
                }

                if (!s->context->loop->data.last_write_failed || us_socket_is_shut_down(0, s)) {
                    us_poll_change(&s->p, us_socket_context(0, s)->loop, us_poll_events(&s->p) & LIBUS_SOCKET_READABLE);
                }
            }

            if (events & LIBUS_SOCKET_READABLE) {
                if (s->context->is_low_prio(s)) {
                    if (s->low_prio_state == 2) {
                        s->low_prio_state = 0;
                    } else if (s->context->loop->data.low_prio_budget > 0) {
                        s->context->loop->data.low_prio_budget--;
                    } else {
                        us_poll_change(&s->p, us_socket_context(0, s)->loop,
                                       us_poll_events(&s->p) & LIBUS_SOCKET_WRITABLE);
                        us_internal_socket_context_unlink_socket(s->context, s);

                        s->prev = 0;
                        s->next = s->context->loop->data.low_prio_head;
                        if (s->next)
                            s->next->prev = s;
                        s->context->loop->data.low_prio_head = s;

                        s->low_prio_state = 1;

                        break;
                    }
                }

                int length;
            read_more:
                length = bsd_recv(us_poll_fd(&s->p), s->context->loop->data.recv_buf + LIBUS_RECV_BUFFER_PADDING,
                                  LIBUS_RECV_BUFFER_LENGTH, 0);
                if (length > 0) {
                    s = s->context->on_data(s, s->context->loop->data.recv_buf + LIBUS_RECV_BUFFER_PADDING, length);

                    if (length == LIBUS_RECV_BUFFER_LENGTH && s && !us_socket_is_closed(0, s)) {
                        goto read_more;
                    }

                } else if (!length) {
                    if (us_socket_is_shut_down(0, s)) {
                        s = us_socket_close(0, s, 0, NULL);
                    } else {
                        us_poll_change(&s->p, us_socket_context(0, s)->loop,
                                       us_poll_events(&s->p) & LIBUS_SOCKET_WRITABLE);
                        s = s->context->on_end(s);
                    }
                } else if (length == LIBUS_SOCKET_ERROR && !bsd_would_block()) {
                    s = us_socket_close(0, s, 0, NULL);
                }
            }
        } break;
    }
}

void us_loop_integrate(struct us_loop_t* loop) {
    us_timer_set(loop->data.sweep_timer, (void (*)(struct us_timer_t*))sweep_timer_cb, LIBUS_TIMEOUT_GRANULARITY * 1000,
                 LIBUS_TIMEOUT_GRANULARITY * 1000);
}

void* us_loop_ext(struct us_loop_t* loop) {
    return loop + 1;
}

#endif
