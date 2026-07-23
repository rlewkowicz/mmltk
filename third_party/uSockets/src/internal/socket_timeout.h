#ifndef LIBUS_INTERNAL_SOCKET_TIMEOUT_H
#define LIBUS_INTERNAL_SOCKET_TIMEOUT_H

static inline void us_internal_socket_timeout(struct us_socket_t* s, unsigned int seconds) {
    s->timeout = seconds ? ((unsigned int)s->context->timestamp + ((seconds + 3) >> 2)) % 240 : 255;
}

static inline void us_internal_socket_long_timeout(struct us_socket_t* s, unsigned int minutes) {
    s->long_timeout = minutes ? ((unsigned int)s->context->long_timestamp + minutes) % 240 : 255;
}

#endif
