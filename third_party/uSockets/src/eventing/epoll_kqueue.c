
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "internal/internal.h"
#include "libusockets.h"

#define GET_READY_POLL(loop, index) (struct us_poll_t*)loop->ready_polls[index].data.ptr
#define SET_READY_POLL(loop, index, poll) loop->ready_polls[index].data.ptr = poll

void us_loop_free(struct us_loop_t* loop) {
    if (!loop) {
        return;
    }
    us_internal_loop_data_free(loop);
    close(loop->fd);
    free(loop);
}

struct us_poll_t* us_create_poll(struct us_loop_t* loop, int fallthrough, unsigned int ext_size) {
    if (!loop) {
        errno = EINVAL;
        return NULL;
    }
    if ((size_t)ext_size > SIZE_MAX - sizeof(struct us_poll_t)) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct us_poll_t* poll = malloc(sizeof(struct us_poll_t) + ext_size);
    if (!poll) {
        errno = ENOMEM;
        return NULL;
    }
    poll->allocation_size = sizeof(struct us_poll_t) + ext_size;
    poll->fallthrough = fallthrough != 0;
    if (!fallthrough) {
        loop->num_polls++;
    }
    return poll;
}

void us_poll_free(struct us_poll_t* p, struct us_loop_t* loop) {
    if (!p->fallthrough) {
        loop->num_polls--;
    }
    free(p);
}

void* us_poll_ext(struct us_poll_t* p) {
    return p + 1;
}

void us_poll_init(struct us_poll_t* p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type) {
    p->state.fd = fd;
    p->state.poll_type = poll_type;
}

int us_poll_events(struct us_poll_t* p) {
    return (int)(((p->state.poll_type & POLL_TYPE_POLLING_IN) ? LIBUS_SOCKET_READABLE : 0U) |
                 ((p->state.poll_type & POLL_TYPE_POLLING_OUT) ? LIBUS_SOCKET_WRITABLE : 0U));
}

LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll_t* p) {
    return p->state.fd;
}

int us_internal_poll_type(struct us_poll_t* p) {
    return p->state.poll_type & 3;
}

void us_internal_poll_set_type(struct us_poll_t* p, int poll_type) {
    p->state.poll_type = poll_type | (p->state.poll_type & 12);
}

void* us_timer_ext(struct us_timer_t* timer) {
    return ((struct us_internal_callback_t*)timer) + 1;
}

struct us_loop_t* us_timer_loop(struct us_timer_t* t) {
    struct us_internal_callback_t* internal_cb = (struct us_internal_callback_t*)t;

    return internal_cb->loop;
}

struct us_loop_t* us_create_loop(void* hint, void (*wakeup_cb)(struct us_loop_t* loop),
                                 void (*pre_cb)(struct us_loop_t* loop), void (*post_cb)(struct us_loop_t* loop),
                                 unsigned int ext_size) {
    if ((size_t)ext_size > SIZE_MAX - sizeof(struct us_loop_t)) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct us_loop_t* loop = (struct us_loop_t*)malloc(sizeof(struct us_loop_t) + ext_size);
    if (!loop) {
        errno = ENOMEM;
        return NULL;
    }
    loop->num_polls = 0;
    loop->num_ready_polls = 0;
    loop->current_ready_poll = 0;

    loop->fd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->fd == -1) {
        const int saved_errno = errno;
        free(loop);
        errno = saved_errno;
        return NULL;
    }

    if (us_internal_loop_data_init(loop, wakeup_cb, pre_cb, post_cb) != 0) {
        const int saved_errno = errno;
        us_internal_loop_data_free(loop);
        close(loop->fd);
        free(loop);
        errno = saved_errno;
        return NULL;
    }
    return loop;
}

int us_loop_run_once(struct us_loop_t* loop, int timeout_ms) {
    if (!loop || timeout_ms < -1) {
        errno = EINVAL;
        return -1;
    }

    us_internal_loop_pre(loop);

    int ready_count;
    do {
        ready_count = epoll_wait(loop->fd, loop->ready_polls, 1024, timeout_ms);
    } while (ready_count == -1 && errno == EINTR && timeout_ms == -1);
    if (ready_count == -1) {
        if (errno == EINTR) {
            us_internal_loop_post(loop);
            return 0;
        }
        us_internal_loop_post(loop);
        return -1;
    }

    loop->num_ready_polls = ready_count;
    for (loop->current_ready_poll = 0; loop->current_ready_poll < loop->num_ready_polls; loop->current_ready_poll++) {
        struct us_poll_t* poll = GET_READY_POLL(loop, loop->current_ready_poll);
        if (poll) {
            uint32_t raw_events = loop->ready_polls[loop->current_ready_poll].events;
            int events = (int)(raw_events & (LIBUS_SOCKET_READABLE | LIBUS_SOCKET_WRITABLE));
            int error = (raw_events & (EPOLLERR | EPOLLHUP)) != 0;
            int fd_watcher_half_close =
                (raw_events & EPOLLRDHUP) != 0 && us_internal_poll_type(poll) == POLL_TYPE_CALLBACK &&
                ((struct us_internal_callback_t*)poll)->cb_expects_the_loop == CALLBACK_KIND_FD_WATCHER;
            events &= us_poll_events(poll);
            if (events || error || fd_watcher_half_close) {
                us_internal_dispatch_ready_poll(poll, error, events, raw_events);
            }
        }
    }
    us_internal_loop_post(loop);
    return ready_count;
}

void us_loop_run(struct us_loop_t* loop) {
    if (!loop) {
        return;
    }
    if (us_loop_integrate(loop) != 0) {
        return;
    }

    while (loop->num_polls) {
        if (us_loop_run_once(loop, -1) == -1) {
            return;
        }
    }
}

void us_internal_loop_update_pending_ready_polls(struct us_loop_t* loop, uintptr_t old_poll_address,
                                                 struct us_poll_t* new_poll) {
    int num_entries_possibly_remaining = 1;

    for (int i = loop->current_ready_poll; i < loop->num_ready_polls && num_entries_possibly_remaining; i++) {
        if ((uintptr_t)GET_READY_POLL(loop, i) == old_poll_address) {
            SET_READY_POLL(loop, i, new_poll);

            num_entries_possibly_remaining--;
        }
    }
}

static inline int us_internal_poll_apply_events(struct us_poll_t* p, struct us_loop_t* loop, int events, int op);

struct us_poll_t* us_poll_resize(struct us_poll_t* p, struct us_loop_t* loop, unsigned int ext_size) {
    if (!p || !loop) {
        errno = EINVAL;
        return NULL;
    }
    if ((size_t)ext_size > SIZE_MAX - sizeof(struct us_poll_t)) {
        errno = EOVERFLOW;
        return NULL;
    }
    int events = us_poll_events(p);
    uintptr_t old_poll_address = (uintptr_t)p;
    const size_t allocation_size = sizeof(struct us_poll_t) + ext_size;

    /* Keep the old allocation alive until epoll accepts the replacement pointer. realloc cannot
     * provide that guarantee: if it moves and EPOLL_CTL_MOD then fails, the kernel retains a
     * dangling data.ptr. */
    struct us_poll_t* new_p = malloc(allocation_size);
    if (!new_p) {
        errno = ENOMEM;
        return NULL;
    }
    memcpy(new_p, p, p->allocation_size < allocation_size ? p->allocation_size : allocation_size);
    new_p->allocation_size = allocation_size;
    if (events && us_internal_poll_apply_events(new_p, loop, events, EPOLL_CTL_MOD) != 0) {
        const int saved_errno = errno;
        free(new_p);
        errno = saved_errno;
        return NULL;
    }

    us_internal_loop_update_pending_ready_polls(loop, old_poll_address, new_p);
    free(p);
    return new_p;
}

/* Applies the requested event mask to the poll and hands it to epoll_ctl with the given
 * operation (EPOLL_CTL_ADD when registering, EPOLL_CTL_MOD when updating). */
static inline int us_internal_poll_apply_events(struct us_poll_t* p, struct us_loop_t* loop, int events, int op) {
    const unsigned int old_poll_type = p->state.poll_type;
    p->state.poll_type = us_internal_poll_type(p) | ((events & LIBUS_SOCKET_READABLE) ? POLL_TYPE_POLLING_IN : 0) |
                         ((events & LIBUS_SOCKET_WRITABLE) ? POLL_TYPE_POLLING_OUT : 0);

    struct epoll_event event = {0};
    event.events = events;
    event.data.ptr = p;
    const int result = epoll_ctl(loop->fd, op, p->state.fd, &event);
    if (result != 0) {
        p->state.poll_type = old_poll_type;
    }
    return result;
}

int us_poll_start(struct us_poll_t* p, struct us_loop_t* loop, int events) {
    return us_internal_poll_apply_events(p, loop, events, EPOLL_CTL_ADD);
}

void us_poll_change(struct us_poll_t* p, struct us_loop_t* loop, int events) {
    int old_events = us_poll_events(p);
    if (old_events != events) {
        (void)us_internal_poll_apply_events(p, loop, events, EPOLL_CTL_MOD);
    }
}

void us_poll_stop(struct us_poll_t* p, struct us_loop_t* loop) {
    (void)epoll_ctl(loop->fd, EPOLL_CTL_DEL, p->state.fd, NULL);

    us_internal_loop_update_pending_ready_polls(loop, (uintptr_t)p, 0);
}

static inline int us_internal_fd_watcher_events(unsigned int events) {
    int native_events = EPOLLRDHUP;
    if (events & LIBUS_FD_READABLE) {
        native_events |= EPOLLIN;
    }
    if (events & LIBUS_FD_WRITABLE) {
        native_events |= EPOLLOUT;
    }
    return native_events;
}

struct us_fd_watcher_t* us_create_fd_watcher(struct us_loop_t* loop, LIBUS_SOCKET_DESCRIPTOR fd, unsigned int events,
                                             unsigned int ext_size,
                                             void (*cb)(struct us_fd_watcher_t* watcher, unsigned int events)) {
    const unsigned int internal_ext_size =
        (unsigned int)(sizeof(struct us_internal_fd_watcher_t) - sizeof(struct us_poll_t));
    if (!loop || fd < 0 || !cb || (events & ~(LIBUS_FD_READABLE | LIBUS_FD_WRITABLE)) != 0) {
        errno = EINVAL;
        return NULL;
    }
    if (ext_size > UINT_MAX - internal_ext_size) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct us_poll_t* poll = us_create_poll(loop, 0, internal_ext_size + ext_size);
    if (!poll) {
        return NULL;
    }
    us_poll_init(poll, fd, POLL_TYPE_CALLBACK);
    struct us_internal_fd_watcher_t* watcher = (struct us_internal_fd_watcher_t*)poll;
    watcher->cb.loop = loop;
    watcher->cb.cb_expects_the_loop = CALLBACK_KIND_FD_WATCHER;
    watcher->cb.leave_poll_ready = 1;
    watcher->cb.cb = NULL;
    watcher->ready_cb = cb;
    if (us_internal_poll_apply_events(poll, loop, us_internal_fd_watcher_events(events), EPOLL_CTL_ADD) != 0) {
        us_poll_free(poll, loop);
        return NULL;
    }
    return (struct us_fd_watcher_t*)watcher;
}

int us_fd_watcher_change(struct us_fd_watcher_t* watcher, unsigned int events) {
    if (!watcher || (events & ~(LIBUS_FD_READABLE | LIBUS_FD_WRITABLE)) != 0) {
        errno = EINVAL;
        return -1;
    }
    struct us_internal_fd_watcher_t* internal = (struct us_internal_fd_watcher_t*)watcher;
    const int native_events = us_internal_fd_watcher_events(events);
    const int old_native_events = us_poll_events(&internal->cb.p);
    unsigned int old_events = 0U;
    if (old_native_events & LIBUS_SOCKET_READABLE) {
        old_events |= LIBUS_FD_READABLE;
    }
    if (old_native_events & LIBUS_SOCKET_WRITABLE) {
        old_events |= LIBUS_FD_WRITABLE;
    }
    if (old_events == events) {
        return 0;
    }
    return us_internal_poll_apply_events(&internal->cb.p, internal->cb.loop, native_events, EPOLL_CTL_MOD);
}

void us_fd_watcher_close(struct us_fd_watcher_t* watcher) {
    if (!watcher) {
        return;
    }
    struct us_internal_fd_watcher_t* internal = (struct us_internal_fd_watcher_t*)watcher;
    us_poll_stop(&internal->cb.p, internal->cb.loop);
    us_poll_free(&internal->cb.p, internal->cb.loop);
}

void* us_fd_watcher_ext(struct us_fd_watcher_t* watcher) {
    return watcher ? ((struct us_internal_fd_watcher_t*)watcher) + 1 : NULL;
}

LIBUS_SOCKET_DESCRIPTOR us_fd_watcher_fd(struct us_fd_watcher_t* watcher) {
    return watcher ? us_poll_fd(&((struct us_internal_fd_watcher_t*)watcher)->cb.p) : -1;
}

unsigned int us_internal_accept_poll_event(struct us_poll_t* p) {
    int fd = us_poll_fd(p);
    uint64_t buf = 0;
    ssize_t bytes_read;
    do {
        bytes_read = read(fd, &buf, sizeof(buf));
    } while (bytes_read == -1 && errno == EINTR);
    return bytes_read == (ssize_t)sizeof(buf) ? (unsigned int)buf : 0;
}

/* Wraps an already opened fd (timerfd or eventfd) in a callback poll. Returns NULL if the poll
 * could not be allocated; the caller owns the fd and must close it in that case. */
static inline struct us_internal_callback_t* us_internal_create_callback(struct us_loop_t* loop, int fallthrough,
                                                                         unsigned int ext_size,
                                                                         LIBUS_SOCKET_DESCRIPTOR fd,
                                                                         enum us_internal_callback_kind kind) {
    const unsigned int internal_ext_size =
        (unsigned int)(sizeof(struct us_internal_callback_t) - sizeof(struct us_poll_t));
    if (ext_size > UINT_MAX - internal_ext_size) {
        errno = EOVERFLOW;
        return NULL;
    }
    struct us_poll_t* p = us_create_poll(loop, fallthrough, internal_ext_size + ext_size);
    if (!p) {
        return NULL;
    }
    us_poll_init(p, fd, POLL_TYPE_CALLBACK);

    struct us_internal_callback_t* cb = (struct us_internal_callback_t*)p;
    cb->loop = loop;
    cb->cb_expects_the_loop = kind;
    cb->leave_poll_ready = 0;

    return cb;
}

/* Unregisters the callback poll, closes its fd and frees the poll. */
static inline void us_internal_close_callback(struct us_internal_callback_t* cb) {
    us_poll_stop(&cb->p, cb->loop);
    close(us_poll_fd(&cb->p));

    us_poll_free(&cb->p, cb->loop);
}

struct us_timer_t* us_create_timer(struct us_loop_t* loop, int fallthrough, unsigned int ext_size) {
    int timerfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timerfd == -1) {
        return NULL;
    }

    struct us_internal_callback_t* cb =
        us_internal_create_callback(loop, fallthrough, ext_size, timerfd, CALLBACK_KIND_TIMER_OR_UDP);
    if (!cb) {
        const int saved_errno = errno;
        close(timerfd);
        errno = saved_errno;
        return NULL;
    }

    return (struct us_timer_t*)cb;
}

void us_timer_close(struct us_timer_t* timer) {
    us_internal_close_callback((struct us_internal_callback_t*)timer);
}

int us_timer_set(struct us_timer_t* t, void (*cb)(struct us_timer_t* t), int ms, int repeat_ms) {
    struct us_internal_callback_t* internal_cb = (struct us_internal_callback_t*)t;

    internal_cb->cb = (void (*)(struct us_internal_callback_t*))cb;

    struct itimerspec timer_spec = {{repeat_ms / 1000, (long)(repeat_ms % 1000) * (long)1000000},
                                    {ms / 1000, (long)(ms % 1000) * (long)1000000}};

    if (timerfd_settime(us_poll_fd((struct us_poll_t*)t), 0, &timer_spec, NULL) != 0) {
        return -1;
    }
    return us_poll_start((struct us_poll_t*)t, internal_cb->loop, LIBUS_SOCKET_READABLE);
}

struct us_internal_async* us_internal_create_async(struct us_loop_t* loop, int fallthrough, unsigned int ext_size) {
    int event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd == -1) {
        return NULL;
    }

    struct us_internal_callback_t* cb =
        us_internal_create_callback(loop, fallthrough, ext_size, event_fd, CALLBACK_KIND_ASYNC);
    if (!cb) {
        const int saved_errno = errno;
        close(event_fd);
        errno = saved_errno;
    }

    return (struct us_internal_async*)cb;
}

void us_internal_async_close(struct us_internal_async* a) {
    us_internal_close_callback((struct us_internal_callback_t*)a);
}

int us_internal_async_set(struct us_internal_async* a, void (*cb)(struct us_internal_async*)) {
    struct us_internal_callback_t* internal_cb = (struct us_internal_callback_t*)a;

    internal_cb->cb = (void (*)(struct us_internal_callback_t*))cb;

    return us_internal_poll_apply_events((struct us_poll_t*)a, internal_cb->loop, LIBUS_SOCKET_READABLE, EPOLL_CTL_ADD);
}

void us_internal_async_wakeup(struct us_internal_async* a) {
    uint64_t one = 1;
    ssize_t bytes_written;
    do {
        bytes_written = write(us_poll_fd((struct us_poll_t*)a), &one, sizeof(one));
    } while (bytes_written == -1 && errno == EINTR);
}
