#ifdef LIBUS_USE_QUIC

#include "internal/networking/bsd.h"

#include "quic.h"

#include "lsquic.h"
#include "lsquic_types.h"
#include "lsxpack_header.h"

#ifndef _WIN32
#include <netinet/in.h>
#include <errno.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void leave_all();

lsquic_engine_t* global_engine;
lsquic_engine_t* global_client_engine;

struct us_quic_socket_context_s {
    struct us_udp_packet_buffer_t* recv_buf;
    int outgoing_packets;

    struct us_loop_t* loop;
    lsquic_engine_t* engine;
    lsquic_engine_t* client_engine;

    us_quic_socket_context_options_t options;

    void (*on_stream_data)(us_quic_stream_t* s, char* data, int length);
    void (*on_stream_end)(us_quic_stream_t* s);
    void (*on_stream_headers)(us_quic_stream_t* s);
    void (*on_stream_open)(us_quic_stream_t* s, int is_client);
    void (*on_stream_close)(us_quic_stream_t* s);
    void (*on_stream_writable)(us_quic_stream_t* s);
    void (*on_open)(us_quic_socket_t* s, int is_client);
    void (*on_close)(us_quic_socket_t* s);
};

void us_quic_socket_context_on_stream_data(us_quic_socket_context_t* context,
                                           void (*on_stream_data)(us_quic_stream_t* s, char* data, int length)) {
    context->on_stream_data = on_stream_data;
}
void us_quic_socket_context_on_stream_end(us_quic_socket_context_t* context,
                                          void (*on_stream_end)(us_quic_stream_t* s)) {
    context->on_stream_end = on_stream_end;
}
void us_quic_socket_context_on_stream_headers(us_quic_socket_context_t* context,
                                              void (*on_stream_headers)(us_quic_stream_t* s)) {
    context->on_stream_headers = on_stream_headers;
}
void us_quic_socket_context_on_stream_open(us_quic_socket_context_t* context,
                                           void (*on_stream_open)(us_quic_stream_t* s, int is_client)) {
    context->on_stream_open = on_stream_open;
}
void us_quic_socket_context_on_stream_close(us_quic_socket_context_t* context,
                                            void (*on_stream_close)(us_quic_stream_t* s)) {
    context->on_stream_close = on_stream_close;
}
void us_quic_socket_context_on_open(us_quic_socket_context_t* context,
                                    void (*on_open)(us_quic_socket_t* s, int is_client)) {
    context->on_open = on_open;
}
void us_quic_socket_context_on_close(us_quic_socket_context_t* context, void (*on_close)(us_quic_socket_t* s)) {
    context->on_close = on_close;
}
void us_quic_socket_context_on_stream_writable(us_quic_socket_context_t* context,
                                               void (*on_stream_writable)(us_quic_stream_t* s)) {
    context->on_stream_writable = on_stream_writable;
}

void on_udp_socket_writable(struct us_udp_socket_t* s) {
    us_quic_socket_context_t* context = us_udp_socket_user(s);

    lsquic_engine_send_unsent_packets(context->engine);
}

static struct sockaddr_storage quic_local_addr_from_packet(struct us_udp_socket_t* s,
                                                           struct us_udp_packet_buffer_t* buf, int index) {
    char ip[16];
    int ip_length = us_udp_packet_buffer_local_ip(buf, index, ip);
    if (!ip_length) {
        printf("We got no ip on received packet!\n");
        exit(0);
    }
    int port = us_udp_socket_bound_port(s);
    struct sockaddr_storage local_addr = {0};
    if (ip_length == 16) {
        struct sockaddr_in6* ipv6 = (struct sockaddr_in6*)&local_addr;
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = ntohs(port);
        memcpy(ipv6->sin6_addr.s6_addr, ip, 16);
    } else {
        struct sockaddr_in* ipv4 = (struct sockaddr_in*)&local_addr;
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = ntohs(port);
        memcpy(&ipv4->sin_addr.s_addr, ip, 4);
    }
    return local_addr;
}

static void quic_process_udp_packets(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf, int packets,
                                     lsquic_engine_t* engine) {
    for (int i = 0; i < packets; i++) {
        char* payload = us_udp_packet_buffer_payload(buf, i);
        int length = us_udp_packet_buffer_payload_length(buf, i);
        int ecn = us_udp_packet_buffer_ecn(buf, i);
        void* peer_addr = us_udp_packet_buffer_peer(buf, i);
        struct sockaddr_storage local_addr = quic_local_addr_from_packet(s, buf, i);
        int ret = lsquic_engine_packet_in(engine, (unsigned char*)payload, length, (struct sockaddr*)&local_addr,
                                          peer_addr, (void*)s, 0);
    }
}

void on_udp_socket_data_client(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf, int packets) {
    int fd = us_poll_fd((struct us_poll_t*)s);

    us_quic_socket_context_t* context = us_udp_socket_user(s);
    quic_process_udp_packets(s, buf, packets, context->client_engine);

    lsquic_engine_process_conns(context->client_engine);
}

void on_udp_socket_data(struct us_udp_socket_t* s, struct us_udp_packet_buffer_t* buf, int packets) {
    us_quic_socket_context_t* context = us_udp_socket_user(s);

    lsquic_engine_process_conns(context->engine);

    quic_process_udp_packets(s, buf, packets, context->engine);

    lsquic_engine_process_conns(context->engine);
}

/* Let's use this on Windows and macOS where it is not defined (todo: put in bsd.h) */
#ifndef UIO_MAXIOV
#define UIO_MAXIOV 1024

#if !defined(_WIN32) && !defined(__FreeBSD__)
struct mmsghdr {
    struct msghdr msg_hdr;
    unsigned int msg_len;
};
#endif
#endif

int send_packets_out(void* ctx, const struct lsquic_out_spec* specs, unsigned n_specs) {
#ifndef _WIN32
    us_quic_socket_context_t* context = ctx;

    struct mmsghdr hdrs[UIO_MAXIOV];
    int run_length = 0;

    struct us_udp_socket_t* last_socket = (struct us_udp_socket_t*)specs[0].peer_ctx;

    int sent = 0;
    for (int i = 0; i < n_specs; i++) {
        if (run_length == UIO_MAXIOV || specs[i].peer_ctx != last_socket) {
            int ret = bsd_sendmmsg(us_poll_fd((struct us_poll_t*)last_socket), hdrs, run_length, 0);
            if (ret != run_length) {
                if (ret == -1) {
                    printf("unhandled udp backpressure!\n");
                    return sent;
                } else {
                    printf("unhandled udp backpressure!\n");
                    errno = EAGAIN;
                    return sent + ret;
                }
            }
            sent += ret;
            run_length = 0;
            last_socket = specs[i].peer_ctx;
        }

        memset(&hdrs[run_length], 0, sizeof(hdrs[run_length]));

        hdrs[run_length].msg_hdr.msg_name = (void*)specs[i].dest_sa;
        hdrs[run_length].msg_hdr.msg_namelen =
            (AF_INET == specs[i].dest_sa->sa_family ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6)),
        hdrs[run_length].msg_hdr.msg_iov = specs[i].iov;
        hdrs[run_length].msg_hdr.msg_iovlen = specs[i].iovlen;
        hdrs[run_length].msg_hdr.msg_flags = 0;

        run_length++;
    }

    if (run_length) {
        int ret = bsd_sendmmsg(us_poll_fd((struct us_poll_t*)last_socket), hdrs, run_length, 0);
        if (ret == -1) {
            printf("backpressure! A\n");
            return sent;
        }
        if (sent + ret != n_specs) {
            printf("backpressure! B\n");
            printf("errno is: %d\n", errno);
            errno = EAGAIN;
        }
        return sent + ret;
    }

#endif

    return n_specs;
}

lsquic_conn_ctx_t* on_new_conn(void* stream_if_ctx, lsquic_conn_t* c) {
    us_quic_socket_context_t* context = stream_if_ctx;

    printf("Context is: %p\n", context);

    int is_client = 0;
    if (lsquic_conn_get_engine(c) == context->client_engine) {
        is_client = 1;
    }

    context->on_open((us_quic_socket_t*)c, is_client);

    return (lsquic_conn_ctx_t*)context;
}

void us_quic_socket_create_stream(us_quic_socket_t* s, int ext_size) {
    lsquic_conn_make_stream((lsquic_conn_t*)s);
}

void on_conn_closed(lsquic_conn_t* c) {
    us_quic_socket_context_t* context = (us_quic_socket_context_t*)lsquic_conn_get_ctx(c);

    printf("on_conn_closed!\n");

    context->on_close((us_quic_socket_t*)c);
}

lsquic_stream_ctx_t* on_new_stream(void* stream_if_ctx, lsquic_stream_t* s) {
    lsquic_stream_wantread(s, 1);

    us_quic_socket_context_t* context = stream_if_ctx;

    // todo: hardcoded for now

    int ext_size = 256;

    void* ext = malloc(ext_size);
    strcpy(ext, "Hello I am ext!");

    int is_client = 0;
    if (lsquic_conn_get_engine(lsquic_stream_conn(s)) == context->client_engine) {
        is_client = 1;
    }

    lsquic_stream_set_ctx(s, ext);
    context->on_stream_open((us_quic_stream_t*)s, is_client);

    return ext;
}

struct header_buf {
    unsigned off;
    char buf[UINT16_MAX];
};

int header_set_ptr(struct lsxpack_header* hdr, struct header_buf* header_buf, const char* name, size_t name_len,
                   const char* val, size_t val_len) {
    if (header_buf->off + name_len + val_len <= sizeof(header_buf->buf)) {
        memcpy(header_buf->buf + header_buf->off, name, name_len);
        memcpy(header_buf->buf + header_buf->off + name_len, val, val_len);
        lsxpack_header_set_offset2(hdr, header_buf->buf + header_buf->off, 0, name_len, name_len, val_len);
        header_buf->off += name_len + val_len;
        return 0;
    } else
        return -1;
}

struct header_buf hbuf;
struct lsxpack_header headers_arr[10];

void us_quic_socket_context_set_header(us_quic_socket_context_t* context, int index, const char* key, int key_length,
                                       const char* value, int value_length) {
    if (header_set_ptr(&headers_arr[index], &hbuf, key, key_length, value, value_length) != 0) {
        printf("CANNOT FORMAT HEADER!\n");
        exit(0);
    }
}

void us_quic_socket_context_send_headers(us_quic_socket_context_t* context, us_quic_stream_t* s, int num,
                                         int has_body) {
    lsquic_http_headers_t headers = {
        .count = num,
        .headers = headers_arr,
    };
    if (lsquic_stream_send_headers((lsquic_stream_t*)s, &headers, has_body ? 0 : 1)) {
        printf("CANNOT SEND HEADERS!\n");
        exit(0);
    }

    hbuf.off = 0;
}

int us_quic_stream_is_client(us_quic_stream_t* s) {
    us_quic_socket_context_t* context =
        (us_quic_socket_context_t*)lsquic_conn_get_ctx(lsquic_stream_conn((lsquic_stream_t*)s));

    int is_client = 0;
    if (lsquic_conn_get_engine(lsquic_stream_conn((lsquic_stream_t*)s)) == context->client_engine) {
        is_client = 1;
    }
    return is_client;
}

us_quic_socket_t* us_quic_stream_socket(us_quic_stream_t* s) {
    return (us_quic_socket_t*)lsquic_stream_conn((lsquic_stream_t*)s);
}

static void on_read(lsquic_stream_t* s, lsquic_stream_ctx_t* h) {
    us_quic_socket_context_t* context = (us_quic_socket_context_t*)lsquic_conn_get_ctx(lsquic_stream_conn(s));

    void* header_set = lsquic_stream_get_hset(s);
    if (header_set) {
        context->on_stream_headers((us_quic_stream_t*)s);
        leave_all();
    }

    char temp[4096] = {0};
    int nr = lsquic_stream_read(s, temp, 4096);

    if (nr == 0) {
        lsquic_stream_wantread(s, 0);
        context->on_stream_end((us_quic_stream_t*)s);
    } else if (nr == -1) {
        if (errno != EWOULDBLOCK) {
            printf("UNHANDLED ON_READ ERROR\n");
            exit(0);
        }
    } else {
        context->on_stream_data((us_quic_stream_t*)s, temp, nr);
    }

    return;

    printf("read returned: %d\n", nr);

    if (nr == -1) {
        printf("Error in reading! errno is: %d\n", errno);
        if (errno != EWOULDBLOCK) {
            printf("Errno is not EWOULDBLOCK\n");
        } else {
            printf("Errno is would block, fine!\n");
        }
        exit(0);
        return;
    }

    if (nr == 0) {
        /* Are we polling for writable (todo: make this check faster)? */
        if (lsquic_stream_wantwrite(s, 1)) {
            printf("we are polling for write, so leaving the stream open!\n");

            lsquic_stream_wantread(s, 0);

        } else {
            lsquic_stream_wantwrite(s, 0);

            lsquic_stream_close(s);
        }

        return;
    }

    printf("on_stream_data: %d\n", nr);
    context->on_stream_data((us_quic_stream_t*)s, temp, nr);
}

int us_quic_stream_write(us_quic_stream_t* s, char* data, int length) {
    lsquic_stream_t* stream = (lsquic_stream_t*)s;
    int ret = lsquic_stream_write((lsquic_stream_t*)s, data, length);
    if (ret != length) {
        lsquic_stream_wantwrite((lsquic_stream_t*)s, 1);
    } else {
        lsquic_stream_wantwrite((lsquic_stream_t*)s, 0);
    }
    return ret;
}

static void on_write(lsquic_stream_t* s, lsquic_stream_ctx_t* h) {
    us_quic_socket_context_t* context = (us_quic_socket_context_t*)lsquic_conn_get_ctx(lsquic_stream_conn(s));

    context->on_stream_writable((us_quic_stream_t*)s);
}

static void on_stream_close(lsquic_stream_t* s, lsquic_stream_ctx_t* h) {}

#include "openssl/ssl.h"

static char s_alpn[0x100];

int add_alpn(const char* alpn) {
    size_t alpn_len, all_len;

    alpn_len = strlen(alpn);
    if (alpn_len > 255)
        return -1;

    all_len = strlen(s_alpn);
    if (all_len + 1 + alpn_len + 1 > sizeof(s_alpn))
        return -1;

    s_alpn[all_len] = alpn_len;
    memcpy(&s_alpn[all_len + 1], alpn, alpn_len);
    s_alpn[all_len + 1 + alpn_len] = '\0';
    return 0;
}

static int select_alpn(SSL* ssl, const unsigned char** out, unsigned char* outlen, const unsigned char* in,
                       unsigned int inlen, void* arg) {
    int r;

    printf("select_alpn\n");

    r = SSL_select_next_proto((unsigned char**)out, outlen, in, inlen, (unsigned char*)s_alpn, strlen(s_alpn));
    if (r == OPENSSL_NPN_NEGOTIATED) {
        printf("OPENSSL_NPN_NEGOTIATED\n");
        return SSL_TLSEXT_ERR_OK;
    } else {
        printf("no supported protocol can be selected!\n");
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
}

SSL_CTX* old_ctx;

int server_name_cb(SSL* s, int* al, void* arg) {
    printf("yolo SNI server_name_cb\n");

    SSL_set_SSL_CTX(s, old_ctx);

    printf("existing name is: %s\n", SSL_get_servername(s, TLSEXT_NAMETYPE_host_name));

    if (!SSL_get_servername(s, TLSEXT_NAMETYPE_host_name)) {
        SSL_set_tlsext_host_name(s, "YOLO NAME!");
        printf("set name is: %s\n", SSL_get_servername(s, TLSEXT_NAMETYPE_host_name));
    }

    return SSL_TLSEXT_ERR_OK;
}

struct ssl_ctx_st* get_ssl_ctx(void* peer_ctx, const struct sockaddr* local) {
    printf("getting ssl ctx now, peer_ctx: %p\n", peer_ctx);

    struct us_udp_socket_t* udp_socket = (struct us_udp_socket_t*)peer_ctx;

    struct us_quic_socket_context_s* context = us_udp_socket_user(udp_socket);

    if (old_ctx) {
        return old_ctx;
    }

    us_quic_socket_context_options_t* options = &context->options;

    SSL_CTX* ctx = SSL_CTX_new(TLS_method());

    old_ctx = ctx;

    SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION);
    SSL_CTX_set_max_proto_version(ctx, TLS1_3_VERSION);

    SSL_CTX_set_alpn_select_cb(ctx, select_alpn, NULL);

    SSL_CTX_set_tlsext_servername_callback(ctx, server_name_cb);

    printf("Key: %s\n", options->key_file_name);
    printf("Cert: %s\n", options->cert_file_name);

    int a = SSL_CTX_use_certificate_chain_file(ctx, options->cert_file_name);
    int b = SSL_CTX_use_PrivateKey_file(ctx, options->key_file_name, SSL_FILETYPE_PEM);

    printf("loaded cert and key? %d, %d\n", a, b);

    return ctx;
}

SSL_CTX* sni_lookup(void* lsquic_cert_lookup_ctx, const struct sockaddr* local, const char* sni) {
    printf("simply returning old ctx in sni\n");
    return old_ctx;
}

int log_buf_cb(void* logger_ctx, const char* buf, size_t len) {
    printf("%.*s\n", (int)len, buf);
    return 0;
}

int us_quic_stream_shutdown_read(us_quic_stream_t* s) {
    lsquic_stream_t* stream = (lsquic_stream_t*)s;

    int ret = lsquic_stream_shutdown((lsquic_stream_t*)s, 0);
    if (ret != 0) {
        printf("cannot shutdown stream!\n");
        exit(0);
    }

    return 0;
}

void* us_quic_stream_ext(us_quic_stream_t* s) {
    return lsquic_stream_get_ctx((lsquic_stream_t*)s);
}

void us_quic_stream_close(us_quic_stream_t* s) {
    lsquic_stream_t* stream = (lsquic_stream_t*)s;

    int ret = lsquic_stream_close((lsquic_stream_t*)s);
    if (ret != 0) {
        printf("cannot close stream!\n");
        exit(0);
    }

    return;
}

int us_quic_stream_shutdown(us_quic_stream_t* s) {
    lsquic_stream_t* stream = (lsquic_stream_t*)s;

    int ret = lsquic_stream_shutdown((lsquic_stream_t*)s, 1);
    if (ret != 0) {
        printf("cannot shutdown stream!\n");
        exit(0);
    }

    return 0;
}

struct header_set_hd {
    int offset;
};

struct header_set_hd* last_hset;

struct processed_header {
    void *name, *value;
    int name_length, value_length;
};

int us_quic_socket_context_get_header(us_quic_socket_context_t* context, int index, char** name, int* name_length,
                                      char** value, int* value_length) {
    if (index < last_hset->offset) {
        struct processed_header* pd = (struct processed_header*)(last_hset + 1);

        pd = pd + index;

        *name = pd->name;
        *value = pd->value;
        *value_length = pd->value_length;
        *name_length = pd->name_length;

        return 1;
    }

    return 0;
}

char pool[1000][4096];
int pool_top = 0;

void* take() {
    if (pool_top == 1000) {
        printf("out of memory\n");
        exit(0);
    }
    return pool[pool_top++];
}

void leave_all() {
    pool_top = 0;
}

void* hsi_create_header_set(void* hsi_ctx, lsquic_stream_t* stream, int is_push_promise) {
    void* hset = take();
    memset(hset, 0, sizeof(struct header_set_hd));

    return hset;
}

void hsi_discard_header_set(void* hdr_set) {
    printf("hsi_discard_header!\n");
}

char header_decode_heap[1024 * 8];
int header_decode_heap_offset = 0;

struct lsxpack_header* hsi_prepare_decode(void* hdr_set, struct lsxpack_header* hdr, size_t space) {
    if (!hdr) {
        char* mem = take();
        hdr = (struct lsxpack_header*)mem;
        memset(hdr, 0, sizeof(struct lsxpack_header));
        hdr->buf = mem + sizeof(struct lsxpack_header);
        lsxpack_header_prepare_decode(hdr, hdr->buf, 0, space);
    } else {
        if (space > 4096 - sizeof(struct lsxpack_header)) {
            printf("not hanlded!\n");
            exit(0);
        }

        hdr->val_len = space;
    }

    return hdr;
}

int hsi_process_header(void* hdr_set, struct lsxpack_header* hdr) {
    struct header_set_hd* hd = hdr_set;
    struct processed_header* proc_hdr = (struct processed_header*)(hd + 1);

    if (!hdr) {
        last_hset = hd;

        return 0;
    }

    proc_hdr[hd->offset].value = &hdr->buf[hdr->val_offset];
    proc_hdr[hd->offset].name = &hdr->buf[hdr->name_offset];
    proc_hdr[hd->offset].value_length = hdr->val_len;
    proc_hdr[hd->offset].name_length = hdr->name_len;

    hd->offset++;

    return 0;
}

void timer_cb(struct us_timer_t* t) {
    lsquic_engine_process_conns(global_engine);
    lsquic_engine_process_conns(global_client_engine);

    lsquic_engine_send_unsent_packets(global_engine);
    lsquic_engine_send_unsent_packets(global_client_engine);
}

us_quic_socket_context_t* us_quic_socket_context(us_quic_socket_t* s) {
    return (us_quic_socket_context_t*)lsquic_conn_get_ctx((lsquic_conn_t*)s);
}

void* us_quic_socket_context_ext(us_quic_socket_context_t* context) {
    return context + 1;
}

us_quic_socket_context_t* us_create_quic_socket_context(struct us_loop_t* loop,
                                                        us_quic_socket_context_options_t options, int ext_size) {
    printf("Creating socket context with ssl: %s\n", options.key_file_name);

    us_quic_socket_context_t* context = malloc(sizeof(struct us_quic_socket_context_s) + ext_size);

    context->options = options;

    context->loop = loop;

    context->recv_buf = us_create_udp_packet_buffer();

    if (0 != lsquic_global_init(LSQUIC_GLOBAL_CLIENT | LSQUIC_GLOBAL_SERVER)) {
        exit(EXIT_FAILURE);
    }

    static struct lsquic_stream_if stream_callbacks = {.on_close = on_stream_close,
                                                       .on_conn_closed = on_conn_closed,
                                                       .on_write = on_write,
                                                       .on_read = on_read,
                                                       .on_new_stream = on_new_stream,
                                                       .on_new_conn = on_new_conn};

    static struct lsquic_hset_if hset_if = {.hsi_discard_header_set = hsi_discard_header_set,
                                            .hsi_create_header_set = hsi_create_header_set,
                                            .hsi_prepare_decode = hsi_prepare_decode,
                                            .hsi_process_header = hsi_process_header};

    add_alpn("h3");

    struct lsquic_engine_api engine_api = {
        .ea_packets_out = send_packets_out,
        .ea_packets_out_ctx = (void*)context,
        .ea_stream_if = &stream_callbacks,
        .ea_stream_if_ctx = context,

        .ea_get_ssl_ctx = get_ssl_ctx,

        .ea_lookup_cert = sni_lookup,
        .ea_cert_lu_ctx = 0,

        .ea_hsi_ctx = 0,
        .ea_hsi_if = &hset_if,
    };

    /// printf("log: %d\n", lsquic_set_log_level("debug"));

    static struct lsquic_logger_if logger = {
        .log_buf = log_buf_cb,
    };

    context->engine = lsquic_engine_new(LSENG_SERVER | LSENG_HTTP, &engine_api);

    struct lsquic_engine_api engine_api_client = {
        .ea_packets_out = send_packets_out,
        .ea_packets_out_ctx = (void*)context,
        .ea_stream_if = &stream_callbacks,
        .ea_stream_if_ctx = context,

        .ea_hsi_ctx = 0,
        .ea_hsi_if = &hset_if,
    };

    context->client_engine = lsquic_engine_new(LSENG_HTTP, &engine_api_client);

    printf("Engine: %p\n", context->engine);
    printf("Client Engine: %p\n", context->client_engine);

    struct us_timer_t* delayTimer = us_create_timer(loop, 0, 0);
    us_timer_set(delayTimer, timer_cb, 50, 50);

    global_engine = context->engine;
    global_client_engine = context->client_engine;

    return context;
}

us_quic_listen_socket_t* us_quic_socket_context_listen(us_quic_socket_context_t* context, const char* host, int port,
                                                       int ext_size) {
    return (us_quic_listen_socket_t*)us_create_udp_socket(context->loop, NULL, on_udp_socket_data,
                                                          on_udp_socket_writable, host, port, context);
}

us_quic_socket_t* us_quic_socket_context_connect(us_quic_socket_context_t* context, const char* host, int port,
                                                 int ext_size) {
    printf("Connecting..\n");

    struct sockaddr_storage storage = {0};

    struct sockaddr_in6* addr = (struct sockaddr_in6*)&storage;
    addr->sin6_addr.s6_addr[15] = 1;
    addr->sin6_port = htons(9004);
    addr->sin6_family = AF_INET6;

    struct us_udp_socket_t* udp_socket =
        us_create_udp_socket(context->loop, NULL, on_udp_socket_data_client, on_udp_socket_writable, 0, 0, context);

    int ephemeral = us_udp_socket_bound_port(udp_socket);

    printf("Connecting with udp socket bound to port: %d\n", ephemeral);

    printf("Client udp socket is: %p\n", udp_socket);

    struct sockaddr_storage local_storage = {0};

    struct sockaddr_in6* local_addr = (struct sockaddr_in6*)&local_storage;
    local_addr->sin6_addr.s6_addr[15] = 1;
    local_addr->sin6_port = htons(ephemeral);
    local_addr->sin6_family = AF_INET6;

    void* client =
        lsquic_engine_connect(context->client_engine, LSQVER_I001, (struct sockaddr*)local_addr, (struct sockaddr*)addr,
                              udp_socket, (lsquic_conn_ctx_t*)udp_socket, "sni", 0, 0, 0, 0, 0);

    printf("Client: %p\n", client);

    lsquic_engine_process_conns(context->client_engine);

    return client;
}

#endif
