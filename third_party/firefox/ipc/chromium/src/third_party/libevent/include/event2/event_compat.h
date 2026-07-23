/*
 * Copyright (c) 2000-2007 Niels Provos <provos@citi.umich.edu>
 * Copyright (c) 2007-2012 Niels Provos and Nick Mathewson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef EVENT2_EVENT_COMPAT_H_INCLUDED_
#define EVENT2_EVENT_COMPAT_H_INCLUDED_

#include <event2/visibility.h>

#ifdef __cplusplus
extern "C" {
#endif

#include <event2/event-config.h>
#ifdef EVENT__HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

#include <event2/util.h>

EVENT2_EXPORT_SYMBOL
struct event_base *event_init(void);

EVENT2_EXPORT_SYMBOL
int event_dispatch(void);

EVENT2_EXPORT_SYMBOL
int event_loop(int);


EVENT2_EXPORT_SYMBOL
int event_loopexit(const struct timeval *);


EVENT2_EXPORT_SYMBOL
int event_loopbreak(void);

EVENT2_EXPORT_SYMBOL
int event_once(evutil_socket_t , short,
    void (*)(evutil_socket_t, short, void *), void *, const struct timeval *);


EVENT2_EXPORT_SYMBOL
const char *event_get_method(void);


EVENT2_EXPORT_SYMBOL
int	event_priority_init(int);

EVENT2_EXPORT_SYMBOL
void event_set(struct event *, evutil_socket_t, short, void (*)(evutil_socket_t, short, void *), void *);

#define evtimer_set(ev, cb, arg)	event_set((ev), -1, 0, (cb), (arg))
#define evsignal_set(ev, x, cb, arg)	\
	event_set((ev), (x), EV_SIGNAL|EV_PERSIST, (cb), (arg))


#define timeout_add(ev, tv)		event_add((ev), (tv))
#define timeout_set(ev, cb, arg)	event_set((ev), -1, 0, (cb), (arg))
#define timeout_del(ev)			event_del(ev)
#define timeout_pending(ev, tv)		event_pending((ev), EV_TIMEOUT, (tv))
#define timeout_initialized(ev)		event_initialized(ev)

#define signal_add(ev, tv)		event_add((ev), (tv))
#define signal_set(ev, x, cb, arg)				\
	event_set((ev), (x), EV_SIGNAL|EV_PERSIST, (cb), (arg))
#define signal_del(ev)			event_del(ev)
#define signal_pending(ev, tv)		event_pending((ev), EV_SIGNAL, (tv))
#define signal_initialized(ev)		event_initialized(ev)

#ifndef EVENT_FD
#define EVENT_FD(ev)		((int)event_get_fd(ev))
#define EVENT_SIGNAL(ev)	event_get_signal(ev)
#endif

#ifdef __cplusplus
}
#endif

#endif /* EVENT2_EVENT_COMPAT_H_INCLUDED_ */
