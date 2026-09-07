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

#ifndef EVENT2_BUFFER_COMPAT_H_INCLUDED_
#define EVENT2_BUFFER_COMPAT_H_INCLUDED_

#include <event2/visibility.h>



EVENT2_EXPORT_SYMBOL
char *evbuffer_readline(struct evbuffer *buffer);

typedef void (*evbuffer_cb)(struct evbuffer *buffer, size_t old_len, size_t new_len, void *arg);

EVENT2_EXPORT_SYMBOL
int evbuffer_setcb(struct evbuffer *buffer, evbuffer_cb cb, void *cbarg);


EVENT2_EXPORT_SYMBOL
unsigned char *evbuffer_find(struct evbuffer *buffer, const unsigned char *what, size_t len);

#define EVBUFFER_LENGTH(x)	evbuffer_get_length(x)
#define EVBUFFER_DATA(x)	evbuffer_pullup((x), -1)

#endif

