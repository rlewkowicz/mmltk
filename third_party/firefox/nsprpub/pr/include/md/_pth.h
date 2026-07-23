/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nspr_pth_defs_h_)
#define nspr_pth_defs_h_

#define _PR_MD_BLOCK_CLOCK_INTERRUPTS()
#define _PR_MD_UNBLOCK_CLOCK_INTERRUPTS()
#define _PR_MD_DISABLE_CLOCK_INTERRUPTS()
#define _PR_MD_ENABLE_CLOCK_INTERRUPTS()

#define _PT_PTHREAD_MUTEXATTR_INIT        pthread_mutexattr_init
#define _PT_PTHREAD_MUTEXATTR_DESTROY     pthread_mutexattr_destroy
#define _PT_PTHREAD_MUTEX_INIT(m, a)      pthread_mutex_init(&(m), &(a))
#if defined(FREEBSD)
#define _PT_PTHREAD_MUTEX_IS_LOCKED(m)    pt_pthread_mutex_is_locked(&(m))
#else
#define _PT_PTHREAD_MUTEX_IS_LOCKED(m)    (EBUSY == pthread_mutex_trylock(&(m)))
#endif
#define _PT_PTHREAD_CONDATTR_INIT         pthread_condattr_init
#define _PT_PTHREAD_CONDATTR_DESTROY      pthread_condattr_destroy
#define _PT_PTHREAD_COND_INIT(m, a)       pthread_cond_init(&(m), &(a))

#if 0 || 0 \
    || defined(LINUX) || defined(__GNU__) || defined(__GLIBC__) \
    || defined(FREEBSD) || defined(NETBSD) || defined(OPENBSD) \
    || defined(NTO) || 0 \
    || defined(RISCOS)
#define _PT_PTHREAD_INVALIDATE_THR_HANDLE(t)  (t) = 0
#define _PT_PTHREAD_THR_HANDLE_IS_INVALID(t)  (t) == 0
#define _PT_PTHREAD_COPY_THR_HANDLE(st, dt)   (dt) = (st)
#else
#error "pthreads is not supported for this architecture"
#endif

#if defined(_PR_PTHREADS)
#define _PT_PTHREAD_ATTR_INIT            pthread_attr_init
#define _PT_PTHREAD_ATTR_DESTROY         pthread_attr_destroy
#define _PT_PTHREAD_CREATE(t, a, f, r)   pthread_create(t, &a, f, r)
#define _PT_PTHREAD_KEY_CREATE           pthread_key_create
#define _PT_PTHREAD_ATTR_SETSCHEDPOLICY  pthread_attr_setschedpolicy
#define _PT_PTHREAD_ATTR_GETSTACKSIZE(a, s) pthread_attr_getstacksize(a, s)
#define _PT_PTHREAD_GETSPECIFIC(k, r)    (r) = pthread_getspecific(k)
#else
#error "Cannot determine pthread strategy"
#endif

#if (0 && !defined(AIX4_3_PLUS)) \
    || defined(LINUX) || defined(__GNU__)|| defined(__GLIBC__) \
    || defined(FREEBSD) || defined(NETBSD) || defined(OPENBSD) \
    || 0
#define PT_NO_SIGTIMEDWAIT
#endif

#if defined(LINUX) || defined(__GNU__) || defined(__GLIBC__) \
    || defined(FREEBSD)
#define PT_PRIO_MIN            sched_get_priority_min(SCHED_OTHER)
#define PT_PRIO_MAX            sched_get_priority_max(SCHED_OTHER)
#elif defined(NTO)
#define PT_PRIO_MIN            0
#define PT_PRIO_MAX            30
#elif defined(OPENBSD)
#define PT_PRIO_MIN            0
#define PT_PRIO_MAX            31
#elif defined(NETBSD) \
    || 0 \
    || defined(RISCOS) 
#define PT_PRIO_MIN            0
#define PT_PRIO_MAX            126
#else
#error "pthreads is not supported for this architecture"
#endif

#if 0 \
    || defined(LINUX) || defined(__GNU__) || defined(__GLIBC__) \
    || defined(FREEBSD) || defined(NETBSD) || defined(OPENBSD) \
    || defined(NTO) || 0 \
    || defined(RISCOS)
#define _PT_PTHREAD_YIELD()             sched_yield()
#else
#error "Need to define _PT_PTHREAD_YIELD for this platform"
#endif

#endif
