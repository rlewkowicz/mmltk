/*
 * Copyright (c) 2009-2012 Niels Provos and Nick Mathewson
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

#if !defined(IOCP_INTERNAL_H_INCLUDED_)
#define IOCP_INTERNAL_H_INCLUDED_

#if defined(__cplusplus)
extern "C" {
#endif

struct event_overlapped;
struct event_iocp_port;
struct evbuffer;
typedef void (*iocp_callback)(struct event_overlapped *, ev_uintptr_t, ev_ssize_t, int success);

struct event_overlapped {
	iocp_callback cb;
};

EVENT2_EXPORT_SYMBOL
void event_overlapped_init_(struct event_overlapped *, iocp_callback cb);

EVENT2_EXPORT_SYMBOL
struct evbuffer *evbuffer_overlapped_new_(evutil_socket_t fd);

evutil_socket_t evbuffer_overlapped_get_fd_(struct evbuffer *buf);

void evbuffer_overlapped_set_fd_(struct evbuffer *buf, evutil_socket_t fd);

EVENT2_EXPORT_SYMBOL
int evbuffer_launch_read_(struct evbuffer *buf, size_t n, struct event_overlapped *ol);

EVENT2_EXPORT_SYMBOL
int evbuffer_launch_write_(struct evbuffer *buf, ev_ssize_t n, struct event_overlapped *ol);

EVENT2_EXPORT_SYMBOL
void evbuffer_commit_read_(struct evbuffer *, ev_ssize_t);
EVENT2_EXPORT_SYMBOL
void evbuffer_commit_write_(struct evbuffer *, ev_ssize_t);

EVENT2_EXPORT_SYMBOL
struct event_iocp_port *event_iocp_port_launch_(int n_cpus);

EVENT2_EXPORT_SYMBOL
int event_iocp_port_associate_(struct event_iocp_port *port, evutil_socket_t fd,
    ev_uintptr_t key);

EVENT2_EXPORT_SYMBOL
int event_iocp_shutdown_(struct event_iocp_port *port, long waitMsec);

EVENT2_EXPORT_SYMBOL
int event_iocp_activate_overlapped_(struct event_iocp_port *port,
    struct event_overlapped *o,
    ev_uintptr_t key, ev_uint32_t n_bytes);

struct event_base;
EVENT2_EXPORT_SYMBOL
struct event_iocp_port *event_base_get_iocp_(struct event_base *base);

EVENT2_EXPORT_SYMBOL
int event_base_start_iocp_(struct event_base *base, int n_cpus);
void event_base_stop_iocp_(struct event_base *base);

EVENT2_EXPORT_SYMBOL
struct bufferevent *bufferevent_async_new_(struct event_base *base,
    evutil_socket_t fd, int options);

void bufferevent_async_set_connected_(struct bufferevent *bev);
int bufferevent_async_can_connect_(struct bufferevent *bev);
int bufferevent_async_connect_(struct bufferevent *bev, evutil_socket_t fd,
	const struct sockaddr *sa, int socklen);

#if defined(__cplusplus)
}
#endif

#endif
