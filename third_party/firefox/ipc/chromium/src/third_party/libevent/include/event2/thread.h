/*
 * Copyright (c) 2008-2012 Niels Provos and Nick Mathewson
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
#if !defined(EVENT2_THREAD_H_INCLUDED_)
#define EVENT2_THREAD_H_INCLUDED_


#include <event2/visibility.h>

#if defined(__cplusplus)
extern "C" {
#endif

#include <event2/event-config.h>

#define EVTHREAD_WRITE	0x04
#define EVTHREAD_READ	0x08
#define EVTHREAD_TRY    0x10

#if !defined(EVENT__DISABLE_THREAD_SUPPORT) || defined(EVENT_IN_DOXYGEN_)

#define EVTHREAD_LOCK_API_VERSION 1

#define EVTHREAD_LOCKTYPE_RECURSIVE 1
#define EVTHREAD_LOCKTYPE_READWRITE 2

struct evthread_lock_callbacks {
	int lock_api_version;
	unsigned supported_locktypes;
	void *(*alloc)(unsigned locktype);
	void (*free)(void *lock, unsigned locktype);
	int (*lock)(unsigned mode, void *lock);
	int (*unlock)(unsigned mode, void *lock);
};

EVENT2_EXPORT_SYMBOL
int evthread_set_lock_callbacks(const struct evthread_lock_callbacks *);

#define EVTHREAD_CONDITION_API_VERSION 1

struct timeval;

struct evthread_condition_callbacks {
	int condition_api_version;
	void *(*alloc_condition)(unsigned condtype);
	void (*free_condition)(void *cond);
	int (*signal_condition)(void *cond, int broadcast);
	int (*wait_condition)(void *cond, void *lock,
	    const struct timeval *timeout);
};

EVENT2_EXPORT_SYMBOL
int evthread_set_condition_callbacks(
	const struct evthread_condition_callbacks *);

EVENT2_EXPORT_SYMBOL
void evthread_set_id_callback(
    unsigned long (*id_fn)(void));

#if (0 && !defined(EVENT__DISABLE_THREAD_SUPPORT)) || defined(EVENT_IN_DOXYGEN_)
EVENT2_EXPORT_SYMBOL
int evthread_use_windows_threads(void);
#define EVTHREAD_USE_WINDOWS_THREADS_IMPLEMENTED 1
#endif

#if defined(EVENT__HAVE_PTHREADS) || defined(EVENT_IN_DOXYGEN_)
EVENT2_EXPORT_SYMBOL
int evthread_use_pthreads(void);
#define EVTHREAD_USE_PTHREADS_IMPLEMENTED 1

#endif

EVENT2_EXPORT_SYMBOL
void evthread_enable_lock_debugging(void);

EVENT2_EXPORT_SYMBOL
void evthread_enable_lock_debuging(void);

#endif

struct event_base;
EVENT2_EXPORT_SYMBOL
int evthread_make_base_notifiable(struct event_base *base);

#if defined(__cplusplus)
}
#endif

#endif
