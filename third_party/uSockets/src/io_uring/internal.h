#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "liburing.h"
#include "internal/socket_context_link.h"

#define MAX_CONNECTIONS 4096
#define BACKLOG 512
#define MAX_MESSAGE_LEN 2048
#define BUFFERS_COUNT MAX_CONNECTIONS

void add_accept(struct io_uring* ring, int fd, struct sockaddr* client_addr, socklen_t* client_len, unsigned flags);
void add_socket_read(struct io_uring* ring, int fd, unsigned gid, size_t size, unsigned flags);
void add_socket_write(struct io_uring* ring, int fd, __u16 bid, size_t size, unsigned flags);
void add_provide_buf(struct io_uring* ring, __u16 bid, unsigned gid);

enum pointer_tags {
    SOCKET_READ,
    SOCKET_WRITE,
    LISTEN_SOCKET_ACCEPT,
    SOCKET_CONNECT,
    LOOP_TIMER = 7,
};

#include <netinet/tcp.h>

void us_internal_loop_link(struct us_loop_t* loop, struct us_socket_context_t* context);
void us_internal_socket_context_link_socket(struct us_socket_context_t* context, struct us_socket_t* s);

struct us_timer_t {
    struct us_loop_t* loop;
    int fd;
    uint64_t buf;
};

struct us_loop_t {
    struct io_uring ring;
    struct io_uring_buf_ring* buf_ring;

    struct us_timer_t* timer;

    struct us_socket_context_t* head;
    struct us_socket_context_t* iterator;

    uint64_t next_timeout;
};

struct us_socket_context_t {
    struct us_loop_t* loop;
    LIBUS_SOCKET_CONTEXT_SHARED_FIELDS

    LIBUS_SOCKET_CONTEXT_CALLBACK_FIELDS
};

struct us_listen_socket_t {
    struct us_socket_context_t* context;
    int socket_ext_size;
};

struct us_socket_t {
    struct us_socket_context_t* context;
    struct us_socket_t *prev, *next;
    unsigned char timeout;
    unsigned char long_timeout;
    int dd;

    char sendBuf[16 * 1024];
};
