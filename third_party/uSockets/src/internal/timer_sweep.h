#ifndef LIBUS_INTERNAL_TIMER_SWEEP_H
#define LIBUS_INTERNAL_TIMER_SWEEP_H

#define LIBUS_INTERNAL_TIMER_SWEEP(loop_data)                                                       \
    do {                                                                                            \
        for ((loop_data)->iterator = (loop_data)->head; (loop_data)->iterator;                      \
             (loop_data)->iterator = (loop_data)->iterator->next) {                                 \
            struct us_socket_context_t* context = (loop_data)->iterator;                            \
            context->global_tick++;                                                                 \
            unsigned char short_ticks = context->timestamp = context->global_tick % 240;            \
            unsigned char long_ticks = context->long_timestamp = (context->global_tick / 15) % 240; \
            struct us_socket_t* s = context->head_sockets;                                          \
            while (s) {                                                                             \
                while (short_ticks != s->timeout && long_ticks != s->long_timeout) {                \
                    s = s->next;                                                                    \
                    if (!s) {                                                                       \
                        break;                                                                      \
                    }                                                                               \
                }                                                                                   \
                if (!s) {                                                                           \
                    break;                                                                          \
                }                                                                                   \
                context->iterator = s;                                                              \
                if (short_ticks == s->timeout) {                                                    \
                    s->timeout = 255;                                                               \
                    context->on_socket_timeout(s);                                                  \
                }                                                                                   \
                if (context->iterator == s && long_ticks == s->long_timeout) {                      \
                    s->long_timeout = 255;                                                          \
                    context->on_socket_long_timeout(s);                                             \
                }                                                                                   \
                s = (s == context->iterator) ? s->next : context->iterator;                         \
            }                                                                                       \
            context->iterator = 0;                                                                  \
        }                                                                                           \
    } while (0)

#endif
