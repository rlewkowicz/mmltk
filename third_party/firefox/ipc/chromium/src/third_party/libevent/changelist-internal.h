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
#ifndef CHANGELIST_INTERNAL_H_INCLUDED_
#define CHANGELIST_INTERNAL_H_INCLUDED_


#include "event2/util.h"

struct event_change {
	evutil_socket_t fd;
	short old_events;

	ev_uint8_t read_change;
	ev_uint8_t write_change;
	ev_uint8_t close_change;
};


#define EV_CHANGE_ADD     0x01
#define EV_CHANGE_DEL     0x02
#define EV_CHANGE_SIGNAL  EV_SIGNAL
#define EV_CHANGE_PERSIST EV_PERSIST
#define EV_CHANGE_ET      EV_ET

#define EVENT_CHANGELIST_FDINFO_SIZE sizeof(int)

void event_changelist_init_(struct event_changelist *changelist);
void event_changelist_remove_all_(struct event_changelist *changelist,
    struct event_base *base);
void event_changelist_freemem_(struct event_changelist *changelist);

int event_changelist_add_(struct event_base *base, evutil_socket_t fd, short old, short events,
    void *p);
int event_changelist_del_(struct event_base *base, evutil_socket_t fd, short old, short events,
    void *p);

#endif
