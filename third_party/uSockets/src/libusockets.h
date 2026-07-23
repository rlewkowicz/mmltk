
#ifndef LIBUSOCKETS_H
#define LIBUSOCKETS_H

#ifndef LIBUS_RECV_BUFFER_LENGTH
#define LIBUS_RECV_BUFFER_LENGTH 524288
#endif

#define LIBUS_TIMEOUT_GRANULARITY 4
#define LIBUS_RECV_BUFFER_PADDING 32
#define LIBUS_EXT_ALIGNMENT 16

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#define LIBUS_SOCKET_DESCRIPTOR SOCKET
#else
#define LIBUS_SOCKET_DESCRIPTOR int
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    LIBUS_LISTEN_DEFAULT,
    LIBUS_LISTEN_EXCLUSIVE_PORT
};

struct us_socket_t;
struct us_timer_t;
struct us_socket_context_t;
struct us_loop_t;
struct us_poll_t;
struct us_udp_socket_t;
struct us_udp_packet_buffer_t;

char* us_socket_send_buffer(int ssl, struct us_socket_t* s);

char* us_udp_packet_buffer_payload(struct us_udp_packet_buffer_t* buf, int index);
int us_udp_packet_buffer_payload_length(struct us_udp_packet_buffer_t* buf, int index);

int us_udp_packet_buffer_local_ip(struct us_udp_packet_buffer_t* buf, int index, char* ip);

int us_udp_socket_bound_port(struct us_udp_socket_t* s);

char* us_udp_packet_buffer_peer(struct us_udp_packet_buffer_t* buf, int index);

int us_udp_packet_buffer_ecn(struct us_udp_packet_buffer_t* buf, int index);

int us_udp_socket_receive(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf);

void us_udp_buffer_set_packet_payload(struct us_udp_packet_buffer_t* send_buf, int index, int offset, void* payload,
                                      int length, void* peer_addr);

int us_udp_socket_send(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf, int num);

struct us_udp_packet_buffer_t* us_create_udp_packet_buffer();

struct us_udp_socket_t* us_create_udp_socket(struct us_loop_t* loop, struct us_udp_packet_buffer_t* buf,
                                             void (*data_cb)(struct us_udp_socket_t*, struct us_udp_packet_buffer_t*,
                                                             int),
                                             void (*drain_cb)(struct us_udp_socket_t*), const char* host,
                                             unsigned short port, void* user);

void* us_udp_socket_user(struct us_udp_socket_t* s);

int us_udp_socket_bind(struct us_udp_socket_t* s, const char* hostname, unsigned int port);

struct us_timer_t* us_create_timer(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);

void* us_timer_ext(struct us_timer_t* timer);

void us_timer_close(struct us_timer_t* timer);

void us_timer_set(struct us_timer_t* timer, void (*cb)(struct us_timer_t* t), int ms, int repeat_ms);

struct us_loop_t* us_timer_loop(struct us_timer_t* t);

struct us_socket_context_options_t {
    const char* key_file_name;
    const char* cert_file_name;
    const char* passphrase;
    const char* dh_params_file_name;
    const char* ca_file_name;
    const char* ssl_ciphers;
    int ssl_prefer_low_memory_usage;
};

unsigned short us_socket_context_timestamp(int ssl, struct us_socket_context_t* context);

void us_socket_context_add_server_name(int ssl, struct us_socket_context_t* context, const char* hostname_pattern,
                                       struct us_socket_context_options_t options, void* user);
void us_socket_context_remove_server_name(int ssl, struct us_socket_context_t* context, const char* hostname_pattern);
void us_socket_context_on_server_name(int ssl, struct us_socket_context_t* context,
                                      void (*cb)(struct us_socket_context_t*, const char* hostname));
void* us_socket_server_name_userdata(int ssl, struct us_socket_t* s);
void* us_socket_context_find_server_name_userdata(int ssl, struct us_socket_context_t* context,
                                                  const char* hostname_pattern);

void* us_socket_context_get_native_handle(int ssl, struct us_socket_context_t* context);

struct us_socket_context_t* us_create_socket_context(int ssl, struct us_loop_t* loop, int ext_size,
                                                     struct us_socket_context_options_t options);

void us_socket_context_free(int ssl, struct us_socket_context_t* context);

void us_socket_context_on_pre_open(int ssl, struct us_socket_context_t* context,
                                   LIBUS_SOCKET_DESCRIPTOR (*on_pre_open)(struct us_socket_context_t* context,
                                                                          LIBUS_SOCKET_DESCRIPTOR fd, char* ip,
                                                                          int ip_length));
void us_socket_context_on_open(int ssl, struct us_socket_context_t* context,
                               struct us_socket_t* (*on_open)(struct us_socket_t* s, int is_client, char* ip,
                                                              int ip_length));
void us_socket_context_on_close(int ssl, struct us_socket_context_t* context,
                                struct us_socket_t* (*on_close)(struct us_socket_t* s, int code, void* reason));
void us_socket_context_on_data(int ssl, struct us_socket_context_t* context,
                               struct us_socket_t* (*on_data)(struct us_socket_t* s, char* data, int length));
void us_socket_context_on_writable(int ssl, struct us_socket_context_t* context,
                                   struct us_socket_t* (*on_writable)(struct us_socket_t* s));
void us_socket_context_on_timeout(int ssl, struct us_socket_context_t* context,
                                  struct us_socket_t* (*on_timeout)(struct us_socket_t* s));
void us_socket_context_on_long_timeout(int ssl, struct us_socket_context_t* context,
                                       struct us_socket_t* (*on_timeout)(struct us_socket_t* s));
void us_socket_context_on_connect_error(int ssl, struct us_socket_context_t* context,
                                        struct us_socket_t* (*on_connect_error)(struct us_socket_t* s, int code));

void us_socket_context_on_end(int ssl, struct us_socket_context_t* context,
                              struct us_socket_t* (*on_end)(struct us_socket_t* s));

void* us_socket_context_ext(int ssl, struct us_socket_context_t* context);

void us_socket_context_close(int ssl, struct us_socket_context_t* context);

struct us_listen_socket_t* us_socket_context_listen(int ssl, struct us_socket_context_t* context, const char* host,
                                                    int port, int options, int socket_ext_size);

struct us_listen_socket_t* us_socket_context_listen_unix(int ssl, struct us_socket_context_t* context, const char* path,
                                                         int options, int socket_ext_size);

void us_listen_socket_close(int ssl, struct us_listen_socket_t* ls);

struct us_socket_t* us_adopt_accepted_socket(int ssl, struct us_socket_context_t* context,
                                             LIBUS_SOCKET_DESCRIPTOR client_fd, unsigned int socket_ext_size,
                                             char* addr_ip, int addr_ip_length);

struct us_socket_t* us_socket_context_connect(int ssl, struct us_socket_context_t* context, const char* host, int port,
                                              const char* source_host, int options, int socket_ext_size);

struct us_socket_t* us_socket_context_connect_unix(int ssl, struct us_socket_context_t* context,
                                                   const char* server_path, int options, int socket_ext_size);

int us_socket_is_established(int ssl, struct us_socket_t* s);

struct us_socket_t* us_socket_close_connecting(int ssl, struct us_socket_t* s);

struct us_loop_t* us_socket_context_loop(int ssl, struct us_socket_context_t* context);

struct us_socket_t* us_socket_context_adopt_socket(int ssl, struct us_socket_context_t* context, struct us_socket_t* s,
                                                   int ext_size);

struct us_socket_context_t* us_create_child_socket_context(int ssl, struct us_socket_context_t* context,
                                                           int context_ext_size);

struct us_loop_t* us_create_loop(void* hint, void (*wakeup_cb)(struct us_loop_t* loop),
                                 void (*pre_cb)(struct us_loop_t* loop), void (*post_cb)(struct us_loop_t* loop),
                                 unsigned int ext_size);

void us_loop_free(struct us_loop_t* loop);

void* us_loop_ext(struct us_loop_t* loop);

void us_loop_run(struct us_loop_t* loop);

void us_wakeup_loop(struct us_loop_t* loop);

void us_loop_integrate(struct us_loop_t* loop);

long long us_loop_iteration_number(struct us_loop_t* loop);

struct us_poll_t* us_create_poll(struct us_loop_t* loop, int fallthrough, unsigned int ext_size);

void us_poll_free(struct us_poll_t* p, struct us_loop_t* loop);

void us_poll_init(struct us_poll_t* p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type);

void us_poll_start(struct us_poll_t* p, struct us_loop_t* loop, int events);
void us_poll_change(struct us_poll_t* p, struct us_loop_t* loop, int events);
void us_poll_stop(struct us_poll_t* p, struct us_loop_t* loop);

int us_poll_events(struct us_poll_t* p);

void* us_poll_ext(struct us_poll_t* p);

LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll_t* p);

struct us_poll_t* us_poll_resize(struct us_poll_t* p, struct us_loop_t* loop, unsigned int ext_size);

void* us_socket_get_native_handle(int ssl, struct us_socket_t* s);

int us_socket_write(int ssl, struct us_socket_t* s, const char* data, int length, int msg_more);

int us_socket_write2(int ssl, struct us_socket_t* s, const char* header, int header_length, const char* payload,
                     int payload_length);

void us_socket_timeout(int ssl, struct us_socket_t* s, unsigned int seconds);

void us_socket_long_timeout(int ssl, struct us_socket_t* s, unsigned int minutes);

void* us_socket_ext(int ssl, struct us_socket_t* s);

struct us_socket_context_t* us_socket_context(int ssl, struct us_socket_t* s);

void us_socket_flush(int ssl, struct us_socket_t* s);

void us_socket_shutdown(int ssl, struct us_socket_t* s);

void us_socket_shutdown_read(int ssl, struct us_socket_t* s);

int us_socket_is_shut_down(int ssl, struct us_socket_t* s);

int us_socket_is_closed(int ssl, struct us_socket_t* s);

struct us_socket_t* us_socket_close(int ssl, struct us_socket_t* s, int code, void* reason);

int us_socket_local_port(int ssl, struct us_socket_t* s);

int us_socket_remote_port(int ssl, struct us_socket_t* s);

void us_socket_remote_address(int ssl, struct us_socket_t* s, char* buf, int* length);

#ifdef __cplusplus
}
#endif

#if !defined(LIBUS_USE_IO_URING) && !defined(LIBUS_USE_EPOLL) && !defined(LIBUS_USE_LIBUV) && \
    !defined(LIBUS_USE_GCD) && !defined(LIBUS_USE_KQUEUE) && !defined(LIBUS_USE_ASIO)
#if defined(_WIN32)
#define LIBUS_USE_LIBUV
#elif defined(__APPLE__) || defined(__FreeBSD__)
#define LIBUS_USE_KQUEUE
#else
#define LIBUS_USE_EPOLL
#endif
#endif

#endif  // LIBUSOCKETS_H
