#ifndef LIBUS_INTERNAL_SOCKET_CONTEXT_LINK_H
#define LIBUS_INTERNAL_SOCKET_CONTEXT_LINK_H

#define LIBUS_SOCKET_CONTEXT_SHARED_FIELDS          \
    uint32_t global_tick;                           \
    unsigned char timestamp;                        \
    unsigned char long_timestamp;                   \
    struct us_socket_t* head_sockets;               \
    struct us_listen_socket_t* head_listen_sockets; \
    struct us_socket_t* iterator;                   \
    struct us_socket_context_t *prev, *next;

#define LIBUS_SOCKET_CONTEXT_CALLBACK_FIELDS                                                     \
    struct us_socket_t* (*on_open)(struct us_socket_t*, int is_client, char* ip, int ip_length); \
    struct us_socket_t* (*on_data)(struct us_socket_t*, char* data, int length);                 \
    struct us_socket_t* (*on_writable)(struct us_socket_t*);                                     \
    struct us_socket_t* (*on_close)(struct us_socket_t*, int code, void* reason);                \
    struct us_socket_t* (*on_socket_timeout)(struct us_socket_t*);                               \
    struct us_socket_t* (*on_socket_long_timeout)(struct us_socket_t*);                          \
    struct us_socket_t* (*on_end)(struct us_socket_t*);                                          \
    struct us_socket_t* (*on_connect_error)(struct us_socket_t*, int code);

#define us_internal_link_socket_to_context(context, s) \
    do {                                               \
        (s)->context = (context);                      \
        (s)->next = (context)->head_sockets;           \
        (s)->prev = 0;                                 \
        if ((context)->head_sockets) {                 \
            (context)->head_sockets->prev = (s);       \
        }                                              \
        (context)->head_sockets = (s);                 \
    } while (0)

#endif
