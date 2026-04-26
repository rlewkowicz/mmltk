/*
 * Authored by Alex Hultman, 2018-2019.
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

#if (defined(LIBUS_USE_OPENSSL) || defined(LIBUS_USE_WOLFSSL))

void* sni_new();
void sni_free(void* sni, void (*cb)(void*));
int sni_add(void* sni, const char* hostname, void* user);
void* sni_remove(void* sni, const char* hostname);
void* sni_find(void* sni, const char* hostname);

#include "libusockets.h"
#include "internal/internal.h"
#include <string.h>

#ifdef LIBUS_USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/dh.h>
#elif LIBUS_USE_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/openssl/ssl.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/dh.h>
#endif

struct loop_ssl_data {
    char *ssl_read_input, *ssl_read_output;
    unsigned int ssl_read_input_length;
    unsigned int ssl_read_input_offset;
    struct us_socket_t* ssl_socket;

    int last_write_was_msg_more;
    int msg_more;

    BIO* shared_rbio;
    BIO* shared_wbio;
    BIO_METHOD* shared_biom;
};

struct us_internal_ssl_socket_context_t {
    struct us_socket_context_t sc;

    SSL_CTX* ssl_context;
    int is_parent;

    struct us_internal_ssl_socket_t* (*on_open)(struct us_internal_ssl_socket_t*, int is_client, char* ip,
                                                int ip_length);
    struct us_internal_ssl_socket_t* (*on_data)(struct us_internal_ssl_socket_t*, char* data, int length);
    struct us_internal_ssl_socket_t* (*on_writable)(struct us_internal_ssl_socket_t*);
    struct us_internal_ssl_socket_t* (*on_close)(struct us_internal_ssl_socket_t*, int code, void* reason);

    void (*on_server_name)(struct us_internal_ssl_socket_context_t*, const char* hostname);

    void* sni;
};

struct us_internal_ssl_socket_t {
    struct us_socket_t s;
    SSL* ssl;
    int ssl_write_wants_read;
    int ssl_read_wants_write;
};

int passphrase_cb(char* buf, int size, int rwflag, void* u) {
    const char* passphrase = (const char*)u;
    size_t passphrase_length = strlen(passphrase);
    memcpy(buf, passphrase, passphrase_length);
    return (int)passphrase_length;
}

int BIO_s_custom_create(BIO* bio) {
    BIO_set_init(bio, 1);
    return 1;
}

long BIO_s_custom_ctrl(BIO* bio, int cmd, long num, void* user) {
    switch (cmd) {
        case BIO_CTRL_FLUSH:
            return 1;
        default:
            return 0;
    }
}

int BIO_s_custom_write(BIO* bio, const char* data, int length) {
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)BIO_get_data(bio);

    loop_ssl_data->last_write_was_msg_more = loop_ssl_data->msg_more || length == 16413;
    int written = us_socket_write(0, loop_ssl_data->ssl_socket, data, length, loop_ssl_data->last_write_was_msg_more);

    if (!written) {
        BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY | BIO_FLAGS_WRITE);
        return -1;
    }

    return written;
}

int BIO_s_custom_read(BIO* bio, char* dst, int length) {
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)BIO_get_data(bio);

    if (!loop_ssl_data->ssl_read_input_length) {
        BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY | BIO_FLAGS_READ);
        return -1;
    }

    if ((unsigned int)length > loop_ssl_data->ssl_read_input_length) {
        length = loop_ssl_data->ssl_read_input_length;
    }

    memcpy(dst, loop_ssl_data->ssl_read_input + loop_ssl_data->ssl_read_input_offset, length);

    loop_ssl_data->ssl_read_input_offset += length;
    loop_ssl_data->ssl_read_input_length -= length;
    return length;
}

struct us_internal_ssl_socket_t* ssl_on_open(struct us_internal_ssl_socket_t* s, int is_client, char* ip,
                                             int ip_length) {
    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

    struct us_loop_t* loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)loop->data.ssl_data;

    s->ssl = SSL_new(context->ssl_context);
    s->ssl_write_wants_read = 0;
    s->ssl_read_wants_write = 0;
    SSL_set_bio(s->ssl, loop_ssl_data->shared_rbio, loop_ssl_data->shared_wbio);

    BIO_up_ref(loop_ssl_data->shared_rbio);
    BIO_up_ref(loop_ssl_data->shared_wbio);

    if (is_client) {
        SSL_set_connect_state(s->ssl);
    } else {
        SSL_set_accept_state(s->ssl);
    }

    return (struct us_internal_ssl_socket_t*)context->on_open(s, is_client, ip, ip_length);
}

struct us_internal_ssl_socket_t* us_internal_ssl_socket_close(struct us_internal_ssl_socket_t* s, int code,
                                                              void* reason) {
    return (struct us_internal_ssl_socket_t*)us_socket_close(0, (struct us_socket_t*)s, code, reason);
}

struct us_internal_ssl_socket_t* ssl_on_close(struct us_internal_ssl_socket_t* s, int code, void* reason) {
    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

    SSL_free(s->ssl);

    return context->on_close(s, code, reason);
}

struct us_internal_ssl_socket_t* ssl_on_end(struct us_internal_ssl_socket_t* s) {
    return us_internal_ssl_socket_close(s, 0, NULL);
}

struct us_internal_ssl_socket_t* ssl_on_data(struct us_internal_ssl_socket_t* s, void* data, int length) {
    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

    struct us_loop_t* loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)loop->data.ssl_data;

    loop_ssl_data->ssl_read_input = data;
    loop_ssl_data->ssl_read_input_length = length;
    loop_ssl_data->ssl_read_input_offset = 0;
    loop_ssl_data->ssl_socket = &s->s;
    loop_ssl_data->msg_more = 0;

    if (us_internal_ssl_socket_is_shut_down(s)) {
        int ret;
        if ((ret = SSL_shutdown(s->ssl)) == 1) {
            return us_internal_ssl_socket_close(s, 0, NULL);
        } else if (ret < 0) {
            int err = SSL_get_error(s->ssl, ret);

            if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                ERR_clear_error();
            }
        }

        return s;
    }

    int read = 0;
restart:
    while (1) {
        int just_read = SSL_read(s->ssl, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING + read,
                                 LIBUS_RECV_BUFFER_LENGTH - read);

        if (just_read <= 0) {
            int err = SSL_get_error(s->ssl, just_read);

            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
                if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                    ERR_clear_error();
                }

                return us_internal_ssl_socket_close(s, 0, NULL);
            } else {
                if (err == SSL_ERROR_WANT_WRITE) {
                    s->ssl_read_wants_write = 1;
                }

                if (loop_ssl_data->ssl_read_input_length) {
                    return us_internal_ssl_socket_close(s, 0, NULL);
                }

                if (!read) {
                    break;
                }

                context = (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

                s = context->on_data(s, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING, read);
                if (us_socket_is_closed(0, &s->s)) {
                    return s;
                }

                break;
            }
        }

        read += just_read;

        if (read == LIBUS_RECV_BUFFER_LENGTH) {
            context = (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

            s = context->on_data(s, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING, read);
            if (us_socket_is_closed(0, &s->s)) {
                return s;
            }

            read = 0;
            goto restart;
        }
    }

    if (s->ssl_write_wants_read) {
        s->ssl_write_wants_read = 0;

        context = (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

        s = (struct us_internal_ssl_socket_t*)context->sc.on_writable(&s->s);
        if (us_socket_is_closed(0, &s->s)) {
            return s;
        }
    }

    if (SSL_get_shutdown(s->ssl) & SSL_RECEIVED_SHUTDOWN) {
        s = us_internal_ssl_socket_close(s, 0, NULL);
    }

    return s;
}

struct us_internal_ssl_socket_t* ssl_on_writable(struct us_internal_ssl_socket_t* s) {
    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

    // todo: cork here so that we efficiently output both from reading and from writing?

    if (s->ssl_read_wants_write) {
        s->ssl_read_wants_write = 0;

        context = (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

        s = (struct us_internal_ssl_socket_t*)context->sc.on_data(&s->s, 0, 0);
    }

    s = context->on_writable(s);

    return s;
}

void us_internal_init_loop_ssl_data(struct us_loop_t* loop) {
    if (!loop->data.ssl_data) {
        struct loop_ssl_data* loop_ssl_data = malloc(sizeof(struct loop_ssl_data));

        loop_ssl_data->ssl_read_output = malloc(LIBUS_RECV_BUFFER_LENGTH + LIBUS_RECV_BUFFER_PADDING * 2);

        OPENSSL_init_ssl(0, NULL);

        loop_ssl_data->shared_biom = BIO_meth_new(BIO_TYPE_MEM, "µS BIO");
        BIO_meth_set_create(loop_ssl_data->shared_biom, BIO_s_custom_create);
        BIO_meth_set_write(loop_ssl_data->shared_biom, BIO_s_custom_write);
        BIO_meth_set_read(loop_ssl_data->shared_biom, BIO_s_custom_read);
        BIO_meth_set_ctrl(loop_ssl_data->shared_biom, BIO_s_custom_ctrl);

        loop_ssl_data->shared_rbio = BIO_new(loop_ssl_data->shared_biom);
        loop_ssl_data->shared_wbio = BIO_new(loop_ssl_data->shared_biom);
        BIO_set_data(loop_ssl_data->shared_rbio, loop_ssl_data);
        BIO_set_data(loop_ssl_data->shared_wbio, loop_ssl_data);

        loop->data.ssl_data = loop_ssl_data;
    }
}

void us_internal_free_loop_ssl_data(struct us_loop_t* loop) {
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)loop->data.ssl_data;

    if (loop_ssl_data) {
        free(loop_ssl_data->ssl_read_output);

        BIO_free(loop_ssl_data->shared_rbio);
        BIO_free(loop_ssl_data->shared_wbio);

        BIO_meth_free(loop_ssl_data->shared_biom);

        free(loop_ssl_data);
    }
}

int ssl_is_low_prio(struct us_internal_ssl_socket_t* s) {
    return SSL_in_init(s->ssl);
}

void* us_internal_ssl_socket_context_get_native_handle(struct us_internal_ssl_socket_context_t* context) {
    return context->ssl_context;
}

struct us_internal_ssl_socket_context_t* us_internal_create_child_ssl_socket_context(
    struct us_internal_ssl_socket_context_t* context, int context_ext_size) {
    struct us_socket_context_options_t options = {0};
    struct us_internal_ssl_socket_context_t* child_context =
        (struct us_internal_ssl_socket_context_t*)us_create_socket_context(
            0, context->sc.loop,
            sizeof(struct us_internal_ssl_socket_context_t) - sizeof(struct us_socket_context_t) + context_ext_size,
            options);

    child_context->ssl_context = context->ssl_context;
    child_context->is_parent = 0;

    return child_context;
}

void free_ssl_context(SSL_CTX* ssl_context) {
    if (!ssl_context) {
        return;
    }

    void* password = SSL_CTX_get_default_passwd_cb_userdata(ssl_context);
    free(password);

    SSL_CTX_free(ssl_context);
}

SSL_CTX* create_ssl_context_from_options(struct us_socket_context_options_t options) {
    SSL_CTX* ssl_context = SSL_CTX_new(TLS_method());

    SSL_CTX_set_read_ahead(ssl_context, 1);
    SSL_CTX_set_mode(ssl_context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION);

    if (options.ssl_prefer_low_memory_usage) {
        SSL_CTX_set_mode(ssl_context, SSL_MODE_RELEASE_BUFFERS);
    }

    if (options.passphrase) {
        SSL_CTX_set_default_passwd_cb_userdata(ssl_context, (void*)strdup(options.passphrase));
        SSL_CTX_set_default_passwd_cb(ssl_context, passphrase_cb);
    }

    if (options.cert_file_name) {
        if (SSL_CTX_use_certificate_chain_file(ssl_context, options.cert_file_name) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    if (options.key_file_name) {
        if (SSL_CTX_use_PrivateKey_file(ssl_context, options.key_file_name, SSL_FILETYPE_PEM) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    if (options.ca_file_name) {
        STACK_OF(X509_NAME) * ca_list;
        ca_list = SSL_load_client_CA_file(options.ca_file_name);
        if (ca_list == NULL) {
            free_ssl_context(ssl_context);
            return NULL;
        }
        SSL_CTX_set_client_CA_list(ssl_context, ca_list);
        if (SSL_CTX_load_verify_locations(ssl_context, options.ca_file_name, NULL) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
        SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, NULL);
    }

    if (options.dh_params_file_name) {
        DH* dh_2048 = NULL;
        FILE* paramfile;
        paramfile = fopen(options.dh_params_file_name, "r");

        if (paramfile) {
            dh_2048 = PEM_read_DHparams(paramfile, NULL, NULL, NULL);
            fclose(paramfile);
        } else {
            free_ssl_context(ssl_context);
            return NULL;
        }

        if (dh_2048 == NULL) {
            free_ssl_context(ssl_context);
            return NULL;
        }

        const long set_tmp_dh = SSL_CTX_set_tmp_dh(ssl_context, dh_2048);
        DH_free(dh_2048);

        if (set_tmp_dh != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }

        if (SSL_CTX_set_cipher_list(ssl_context,
                                    "DHE-RSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:"
                                    "ECDHE-RSA-AES128-GCM-SHA256") != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    if (options.ssl_ciphers) {
        if (SSL_CTX_set_cipher_list(ssl_context, options.ssl_ciphers) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    return ssl_context;
}

void* us_internal_ssl_socket_context_find_server_name_userdata(struct us_internal_ssl_socket_context_t* context,
                                                               const char* hostname_pattern) {
    printf("finding %s\n", hostname_pattern);

    SSL_CTX* ssl_context = sni_find(context->sni, hostname_pattern);

    if (ssl_context) {
        return SSL_CTX_get_ex_data(ssl_context, 0);
    }

    return 0;
}

void* us_internal_ssl_socket_get_sni_userdata(struct us_internal_ssl_socket_t* s) {
    return SSL_CTX_get_ex_data(SSL_get_SSL_CTX(s->ssl), 0);
}

void us_internal_ssl_socket_context_add_server_name(struct us_internal_ssl_socket_context_t* context,
                                                    const char* hostname_pattern,
                                                    struct us_socket_context_options_t options, void* user) {
    SSL_CTX* ssl_context = create_ssl_context_from_options(options);

    if (ssl_context) {
        if (1 != SSL_CTX_set_ex_data(ssl_context, 0, user)) {
            printf("CANNOT SET EX DATA!\n");
        }

        if (sni_add(context->sni, hostname_pattern, ssl_context)) {
            free_ssl_context(ssl_context);
        }
    }
}

void us_internal_ssl_socket_context_on_server_name(struct us_internal_ssl_socket_context_t* context,
                                                   void (*cb)(struct us_internal_ssl_socket_context_t*,
                                                              const char* hostname)) {
    context->on_server_name = cb;
}

void us_internal_ssl_socket_context_remove_server_name(struct us_internal_ssl_socket_context_t* context,
                                                       const char* hostname_pattern) {
    SSL_CTX* sni_node_ssl_context = (SSL_CTX*)sni_remove(context->sni, hostname_pattern);
    free_ssl_context(sni_node_ssl_context);
}

SSL_CTX* resolve_context(struct us_internal_ssl_socket_context_t* context, const char* hostname) {
    void* user = sni_find(context->sni, hostname);
    if (!user) {
        if (!context->on_server_name) {
            return NULL;
        }

        context->on_server_name(context, hostname);

        user = sni_find(context->sni, hostname);
    }

    return user;
}

int sni_cb(SSL* ssl, int* al, void* arg) {
    if (ssl) {
        const char* hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (hostname && hostname[0]) {
            SSL_CTX* resolved_ssl_context = resolve_context((struct us_internal_ssl_socket_context_t*)arg, hostname);
            if (resolved_ssl_context) {
                SSL_set_SSL_CTX(ssl, resolved_ssl_context);
            } else {
            }
        }

        return SSL_TLSEXT_ERR_OK;
    }

    return SSL_TLSEXT_ERR_NOACK;
}

struct us_internal_ssl_socket_context_t* us_internal_create_ssl_socket_context(
    struct us_loop_t* loop, int context_ext_size, struct us_socket_context_options_t options) {
    us_internal_init_loop_ssl_data(loop);

    SSL_CTX* ssl_context = create_ssl_context_from_options(options);
    if (!ssl_context) {
        return NULL;
    }

    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_create_socket_context(
            0, loop,
            sizeof(struct us_internal_ssl_socket_context_t) - sizeof(struct us_socket_context_t) + context_ext_size,
            options);

    context->on_server_name = NULL;

    context->ssl_context = ssl_context;
    context->is_parent = 1;

    context->sc.is_low_prio = (int (*)(struct us_socket_t*))ssl_is_low_prio;

    SSL_CTX_set_tlsext_servername_callback(context->ssl_context, sni_cb);
    SSL_CTX_set_tlsext_servername_arg(context->ssl_context, context);

    context->sni = sni_new();

    return context;
}

void sni_hostname_destructor(void* user) {
    free_ssl_context((SSL_CTX*)user);
}

void us_internal_ssl_socket_context_free(struct us_internal_ssl_socket_context_t* context) {
    if (context->is_parent) {
        free_ssl_context(context->ssl_context);

        sni_free(context->sni, sni_hostname_destructor);
    }

    us_socket_context_free(0, &context->sc);
}

struct us_listen_socket_t* us_internal_ssl_socket_context_listen(struct us_internal_ssl_socket_context_t* context,
                                                                 const char* host, int port, int options,
                                                                 int socket_ext_size) {
    return us_socket_context_listen(
        0, &context->sc, host, port, options,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

struct us_listen_socket_t* us_internal_ssl_socket_context_listen_unix(struct us_internal_ssl_socket_context_t* context,
                                                                      const char* path, int options,
                                                                      int socket_ext_size) {
    return us_socket_context_listen_unix(
        0, &context->sc, path, options,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

struct us_internal_ssl_socket_t* us_internal_ssl_adopt_accepted_socket(struct us_internal_ssl_socket_context_t* context,
                                                                       LIBUS_SOCKET_DESCRIPTOR accepted_fd,
                                                                       unsigned int socket_ext_size, char* addr_ip,
                                                                       int addr_ip_length) {
    return (struct us_internal_ssl_socket_t*)us_adopt_accepted_socket(
        0, &context->sc, accepted_fd,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size, addr_ip,
        addr_ip_length);
}

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_connect(
    struct us_internal_ssl_socket_context_t* context, const char* host, int port, const char* source_host, int options,
    int socket_ext_size) {
    return (struct us_internal_ssl_socket_t*)us_socket_context_connect(
        0, &context->sc, host, port, source_host, options,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_connect_unix(
    struct us_internal_ssl_socket_context_t* context, const char* server_path, int options, int socket_ext_size) {
    return (struct us_internal_ssl_socket_t*)us_socket_context_connect_unix(
        0, &context->sc, server_path, options,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

void us_internal_ssl_socket_context_on_open(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_open)(struct us_internal_ssl_socket_t* s, int is_client, char* ip,
                                                int ip_length)) {
    us_socket_context_on_open(0, &context->sc,
                              (struct us_socket_t * (*)(struct us_socket_t*, int, char*, int)) ssl_on_open);
    context->on_open = on_open;
}

void us_internal_ssl_socket_context_on_close(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_close)(struct us_internal_ssl_socket_t* s, int code, void* reason)) {
    us_socket_context_on_close(0, (struct us_socket_context_t*)context,
                               (struct us_socket_t * (*)(struct us_socket_t*, int, void*)) ssl_on_close);
    context->on_close = on_close;
}

void us_internal_ssl_socket_context_on_data(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_data)(struct us_internal_ssl_socket_t* s, char* data, int length)) {
    us_socket_context_on_data(0, (struct us_socket_context_t*)context,
                              (struct us_socket_t * (*)(struct us_socket_t*, char*, int)) ssl_on_data);
    context->on_data = on_data;
}

void us_internal_ssl_socket_context_on_writable(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_writable)(struct us_internal_ssl_socket_t* s)) {
    us_socket_context_on_writable(0, (struct us_socket_context_t*)context,
                                  (struct us_socket_t * (*)(struct us_socket_t*)) ssl_on_writable);
    context->on_writable = on_writable;
}

void us_internal_ssl_socket_context_on_timeout(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_timeout)(struct us_internal_ssl_socket_t* s)) {
    us_socket_context_on_timeout(0, (struct us_socket_context_t*)context,
                                 (struct us_socket_t * (*)(struct us_socket_t*)) on_timeout);
}

void us_internal_ssl_socket_context_on_long_timeout(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_long_timeout)(struct us_internal_ssl_socket_t* s)) {
    us_socket_context_on_long_timeout(0, (struct us_socket_context_t*)context,
                                      (struct us_socket_t * (*)(struct us_socket_t*)) on_long_timeout);
}

void us_internal_ssl_socket_context_on_end(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_end)(struct us_internal_ssl_socket_t*)) {
    us_socket_context_on_end(0, (struct us_socket_context_t*)context,
                             (struct us_socket_t * (*)(struct us_socket_t*)) ssl_on_end);
}

void us_internal_ssl_socket_context_on_connect_error(
    struct us_internal_ssl_socket_context_t* context,
    struct us_internal_ssl_socket_t* (*on_connect_error)(struct us_internal_ssl_socket_t*, int code)) {
    us_socket_context_on_connect_error(0, (struct us_socket_context_t*)context,
                                       (struct us_socket_t * (*)(struct us_socket_t*, int)) on_connect_error);
}

void* us_internal_ssl_socket_context_ext(struct us_internal_ssl_socket_context_t* context) {
    return context + 1;
}

void* us_internal_ssl_socket_get_native_handle(struct us_internal_ssl_socket_t* s) {
    return s->ssl;
}

int us_internal_ssl_socket_write(struct us_internal_ssl_socket_t* s, const char* data, int length, int msg_more) {
    if (us_socket_is_closed(0, &s->s) || us_internal_ssl_socket_is_shut_down(s)) {
        return 0;
    }

    struct us_internal_ssl_socket_context_t* context =
        (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);

    struct us_loop_t* loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)loop->data.ssl_data;

    loop_ssl_data->ssl_read_input_length = 0;

    loop_ssl_data->ssl_socket = &s->s;
    loop_ssl_data->msg_more = msg_more;
    loop_ssl_data->last_write_was_msg_more = 0;
    int written = SSL_write(s->ssl, data, length);
    loop_ssl_data->msg_more = 0;

    if (loop_ssl_data->last_write_was_msg_more && !msg_more) {
        us_socket_flush(0, &s->s);
    }

    if (written > 0) {
        return written;
    } else {
        int err = SSL_get_error(s->ssl, written);
        if (err == SSL_ERROR_WANT_READ) {
            s->ssl_write_wants_read = 1;
        } else if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
            ERR_clear_error();
        }

        return 0;
    }
}

void* us_internal_ssl_socket_ext(struct us_internal_ssl_socket_t* s) {
    return s + 1;
}

int us_internal_ssl_socket_is_shut_down(struct us_internal_ssl_socket_t* s) {
    return us_socket_is_shut_down(0, &s->s) || SSL_get_shutdown(s->ssl) & SSL_SENT_SHUTDOWN;
}

void us_internal_ssl_socket_shutdown(struct us_internal_ssl_socket_t* s) {
    if (!us_socket_is_closed(0, &s->s) && !us_internal_ssl_socket_is_shut_down(s)) {
        struct us_internal_ssl_socket_context_t* context =
            (struct us_internal_ssl_socket_context_t*)us_socket_context(0, &s->s);
        struct us_loop_t* loop = us_socket_context_loop(0, &context->sc);
        struct loop_ssl_data* loop_ssl_data = (struct loop_ssl_data*)loop->data.ssl_data;

        loop_ssl_data->ssl_read_input_length = 0;

        loop_ssl_data->ssl_socket = &s->s;

        loop_ssl_data->msg_more = 0;

        int ret = SSL_shutdown(s->ssl);
        if (ret == 0) {
            ret = SSL_shutdown(s->ssl);
        }

        if (ret < 0) {
            int err = SSL_get_error(s->ssl, ret);
            if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                ERR_clear_error();
            }

            us_socket_shutdown(0, &s->s);
        }
    }
}

struct us_internal_ssl_socket_t* us_internal_ssl_socket_context_adopt_socket(
    struct us_internal_ssl_socket_context_t* context, struct us_internal_ssl_socket_t* s, int ext_size) {
    // todo: this is completely untested
    return (struct us_internal_ssl_socket_t*)us_socket_context_adopt_socket(
        0, &context->sc, &s->s, sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + ext_size);
}

#endif
