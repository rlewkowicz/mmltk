/*
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
#ifndef EVMAP_INTERNAL_H_INCLUDED_
#define EVMAP_INTERNAL_H_INCLUDED_


struct event_base;
struct event;

void evmap_io_initmap_(struct event_io_map* ctx);
void evmap_signal_initmap_(struct event_signal_map* ctx);

void evmap_io_clear_(struct event_io_map* ctx);
void evmap_signal_clear_(struct event_signal_map* ctx);

int evmap_io_add_(struct event_base *base, evutil_socket_t fd, struct event *ev);
int evmap_io_del_(struct event_base *base, evutil_socket_t fd, struct event *ev);
void evmap_io_active_(struct event_base *base, evutil_socket_t fd, short events);


int evmap_signal_add_(struct event_base *base, int signum, struct event *ev);
int evmap_signal_del_(struct event_base *base, int signum, struct event *ev);
void evmap_signal_active_(struct event_base *base, evutil_socket_t signum, int ncalls);

void *evmap_io_get_fdinfo_(struct event_io_map *ctx, evutil_socket_t fd);

int evmap_reinit_(struct event_base *base);

void evmap_delete_all_(struct event_base *base);

void evmap_check_integrity_(struct event_base *base);

int evmap_foreach_event_(struct event_base *base,
    event_base_foreach_event_cb fn,
    void *arg);

#endif /* EVMAP_INTERNAL_H_INCLUDED_ */
