/*
 * Copyright (c) 2007-2012 Niels Provos, Nick Mathewson
 * Copyright (c) 2000-2007 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
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
#ifndef EVENT2_BUFFEREVENT_COMPAT_H_INCLUDED_
#define EVENT2_BUFFEREVENT_COMPAT_H_INCLUDED_

#include <event2/visibility.h>

#define evbuffercb bufferevent_data_cb
#define everrorcb bufferevent_event_cb

EVENT2_EXPORT_SYMBOL
struct bufferevent *bufferevent_new(evutil_socket_t fd,
    evbuffercb readcb, evbuffercb writecb, everrorcb errorcb, void *cbarg);


EVENT2_EXPORT_SYMBOL
void bufferevent_settimeout(struct bufferevent *bufev,
    int timeout_read, int timeout_write);

#define EVBUFFER_READ		BEV_EVENT_READING
#define EVBUFFER_WRITE		BEV_EVENT_WRITING
#define EVBUFFER_EOF		BEV_EVENT_EOF
#define EVBUFFER_ERROR		BEV_EVENT_ERROR
#define EVBUFFER_TIMEOUT	BEV_EVENT_TIMEOUT

#define EVBUFFER_INPUT(x)	bufferevent_get_input(x)
#define EVBUFFER_OUTPUT(x)	bufferevent_get_output(x)

#endif
