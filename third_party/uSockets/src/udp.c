
#ifndef LIBUS_USE_IO_URING

#include "libusockets.h"
#include "internal/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int us_udp_packet_buffer_ecn(struct us_udp_packet_buffer_t* buf, int index) {
    return bsd_udp_packet_buffer_ecn(buf, index);
}

int us_udp_packet_buffer_local_ip(struct us_udp_packet_buffer_t* buf, int index, char* ip) {
    return bsd_udp_packet_buffer_local_ip(buf, index, ip);
}

char* us_udp_packet_buffer_peer(struct us_udp_packet_buffer_t* buf, int index) {
    return bsd_udp_packet_buffer_peer(buf, index);
}

char* us_udp_packet_buffer_payload(struct us_udp_packet_buffer_t* buf, int index) {
    return bsd_udp_packet_buffer_payload(buf, index);
}

int us_udp_packet_buffer_payload_length(struct us_udp_packet_buffer_t* buf, int index) {
    return bsd_udp_packet_buffer_payload_length(buf, index);
}

int us_udp_socket_send(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf, int num) {
    int fd = us_poll_fd((struct us_poll_t*)s);

    return bsd_sendmmsg(fd, buf, num, 0);
}

int us_udp_socket_receive(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf) {
    int fd = us_poll_fd((struct us_poll_t*)s);
    return bsd_recvmmsg(fd, buf, LIBUS_UDP_MAX_NUM, 0, 0);
}

void us_udp_buffer_set_packet_payload(struct us_udp_packet_buffer_t* send_buf, int index, int offset, void* payload,
                                      int length, void* peer_addr) {
    bsd_udp_buffer_set_packet_payload(send_buf, index, offset, payload, length, peer_addr);
}

struct us_udp_packet_buffer_t* us_create_udp_packet_buffer() {
    return (struct us_udp_packet_buffer_t*)bsd_create_udp_packet_buffer();
}

struct us_internal_udp_t {
    struct us_internal_callback_t cb;
    struct us_udp_packet_buffer_t* receive_buf;
    void (*data_cb)(struct us_udp_socket_t*, struct us_udp_packet_buffer_t*, int);
    void (*drain_cb)(struct us_udp_socket_t*);
    void* user;
    int port;
};

int us_udp_socket_bound_port(struct us_udp_socket_t* s) {
    return ((struct us_internal_udp_t*)s)->port;
}

void internal_on_udp_read(struct us_udp_socket_t* s) {
    struct us_internal_udp_t* udp = (struct us_internal_udp_t*)s;

    int packets = us_udp_socket_receive(s, udp->receive_buf);

    udp->data_cb(s, udp->receive_buf, packets);
}

void* us_udp_socket_user(struct us_udp_socket_t* s) {
    struct us_internal_udp_t* udp = (struct us_internal_udp_t*)s;

    return udp->user;
}

struct us_udp_socket_t* us_create_udp_socket(struct us_loop_t* loop, struct us_udp_packet_buffer_t* buf,
                                             void (*data_cb)(struct us_udp_socket_t*, struct us_udp_packet_buffer_t*,
                                                             int),
                                             void (*drain_cb)(struct us_udp_socket_t*), const char* host,
                                             unsigned short port, void* user) {
    LIBUS_SOCKET_DESCRIPTOR fd = bsd_create_udp_socket(host, port);
    if (fd == LIBUS_SOCKET_ERROR) {
        return 0;
    }

    if (!buf) {
        buf = us_create_udp_packet_buffer();
    }

    int ext_size = 0;
    int fallthrough = 0;

    struct us_poll_t* p =
        us_create_poll(loop, fallthrough, sizeof(struct us_internal_udp_t) - sizeof(struct us_poll_t) + ext_size);
    us_poll_init(p, fd, POLL_TYPE_CALLBACK);

    struct us_internal_udp_t* cb = (struct us_internal_udp_t*)p;
    cb->cb.loop = loop;
    cb->cb.cb_expects_the_loop = 0;
    cb->cb.leave_poll_ready = 1;

    struct bsd_addr_t tmp;
    bsd_local_addr(fd, &tmp);
    cb->port = bsd_addr_get_port(&tmp);

    printf("The port of UDP is: %d\n", cb->port);

    cb->user = user;

    cb->data_cb = data_cb;
    cb->receive_buf = buf;
    cb->drain_cb = drain_cb;

    cb->cb.cb = (void (*)(struct us_internal_callback_t*))internal_on_udp_read;

    us_poll_start((struct us_poll_t*)cb, cb->cb.loop, LIBUS_SOCKET_READABLE);

    return (struct us_udp_socket_t*)cb;
}

#endif