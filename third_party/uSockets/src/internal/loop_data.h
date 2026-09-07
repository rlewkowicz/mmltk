
#ifndef LOOP_DATA_H
#define LOOP_DATA_H

/* File-scope declarations so the function-pointer members below refer to the
 * real types instead of implicitly declaring new prototype-scoped structs. */
struct us_loop_t;
struct us_socket_context_t;
struct us_socket_t;
struct us_timer_t;
struct us_internal_async;

struct us_internal_loop_data_t {
    struct us_timer_t* sweep_timer;
    struct us_internal_async* wakeup_async;
    int last_write_failed;
    struct us_socket_context_t* head;
    struct us_socket_context_t* iterator;
    char* recv_buf;
    void (*pre_cb)(struct us_loop_t*);
    void (*post_cb)(struct us_loop_t*);
    struct us_socket_t* closed_head;
    struct us_socket_t* low_prio_head;
    int low_prio_budget;
    long long iteration_nr;
};

#endif  // LOOP_DATA_H
