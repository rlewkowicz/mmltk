
#ifdef LIBUS_USE_IO_URING

#include "libusockets.h"
#include "internal/internal.h"
#include "internal/socket_timeout.h"
#include "internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>

int us_socket_local_port(int ssl, struct us_socket_t* s) {
    return 0;
}

int us_socket_remote_port(int ssl, struct us_socket_t* s) {
    return 0;
}

void us_socket_shutdown_read(int ssl, struct us_socket_t* s) {}

void us_socket_remote_address(int ssl, struct us_socket_t* s, char* buf, int* length) {}

struct us_socket_context_t* us_socket_context(int ssl, struct us_socket_t* s) {
    return s->context;
}

void us_socket_timeout(int ssl, struct us_socket_t* s, unsigned int seconds) {
    us_internal_socket_timeout(s, seconds);
}

void us_socket_long_timeout(int ssl, struct us_socket_t* s, unsigned int minutes) {
    us_internal_socket_long_timeout(s, minutes);
}

void us_socket_flush(int ssl, struct us_socket_t* s) {}

int us_socket_is_closed(int ssl, struct us_socket_t* s) {
    return 0;
}

int us_socket_is_established(int ssl, struct us_socket_t* s) {
    return 1;
}

struct us_socket_t* us_socket_close_connecting(int ssl, struct us_socket_t* s) {
    return s;
}

int us_socket_write2(int ssl, struct us_socket_t* s, const char* header, int header_length, const char* payload,
                     int payload_length) {
    exit(1);
}

char* us_socket_send_buffer(int ssl, struct us_socket_t* s) {
    return s->sendBuf;
}

struct us_socket_t* us_socket_close(int ssl, struct us_socket_t* s, int code, void* reason) {
    return s;
}

void* us_socket_get_native_handle(int ssl, struct us_socket_t* s) {
    return 0;
}

int us_socket_write(int ssl, struct us_socket_t* s, const char* data, int length, int msg_more) {
    if (data != s->sendBuf) {
        memcpy(s->sendBuf, data, length);
    }

    struct io_uring_sqe* sqe = io_uring_get_sqe(&s->context->loop->ring);

    io_uring_prep_send(sqe, s->dd, s->sendBuf, length, 0);

    io_uring_sqe_set_flags(sqe, IOSQE_FIXED_FILE);
    io_uring_sqe_set_data(sqe, (char*)s + SOCKET_WRITE);

    return length;
}

void* us_socket_ext(int ssl, struct us_socket_t* s) {
    return s + 1;
}

int us_socket_is_shut_down(int ssl, struct us_socket_t* s) {
    return 0;
}

void us_socket_shutdown(int ssl, struct us_socket_t* s) {}

#endif
