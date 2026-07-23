/**
 * Copyright (c) 2016 Tino Reichardt
 * All rights reserved.
 *
 * You can contact the author at:
 * - zstdmt source repository: https://github.com/mcmilk/zstdmt
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */

#if !defined(THREADING_H_938743)
#define THREADING_H_938743

#include "debug.h"

#if defined(ZSTD_MULTITHREAD)    /* posix assumed ; need a better detection method */
#  include <pthread.h>

#if DEBUGLEVEL < 1

#define ZSTD_pthread_mutex_t            pthread_mutex_t
#define ZSTD_pthread_mutex_init(a, b)   pthread_mutex_init((a), (b))
#define ZSTD_pthread_mutex_destroy(a)   pthread_mutex_destroy((a))
#define ZSTD_pthread_mutex_lock(a)      pthread_mutex_lock((a))
#define ZSTD_pthread_mutex_unlock(a)    pthread_mutex_unlock((a))

#define ZSTD_pthread_cond_t             pthread_cond_t
#define ZSTD_pthread_cond_init(a, b)    pthread_cond_init((a), (b))
#define ZSTD_pthread_cond_destroy(a)    pthread_cond_destroy((a))
#define ZSTD_pthread_cond_wait(a, b)    pthread_cond_wait((a), (b))
#define ZSTD_pthread_cond_signal(a)     pthread_cond_signal((a))
#define ZSTD_pthread_cond_broadcast(a)  pthread_cond_broadcast((a))

#define ZSTD_pthread_t                  pthread_t
#define ZSTD_pthread_create(a, b, c, d) pthread_create((a), (b), (c), (d))
#define ZSTD_pthread_join(a)         pthread_join((a),NULL)

#else


#define ZSTD_pthread_mutex_t            pthread_mutex_t*
int ZSTD_pthread_mutex_init(ZSTD_pthread_mutex_t* mutex, pthread_mutexattr_t const* attr);
int ZSTD_pthread_mutex_destroy(ZSTD_pthread_mutex_t* mutex);
#define ZSTD_pthread_mutex_lock(a)      pthread_mutex_lock(*(a))
#define ZSTD_pthread_mutex_unlock(a)    pthread_mutex_unlock(*(a))

#define ZSTD_pthread_cond_t             pthread_cond_t*
int ZSTD_pthread_cond_init(ZSTD_pthread_cond_t* cond, pthread_condattr_t const* attr);
int ZSTD_pthread_cond_destroy(ZSTD_pthread_cond_t* cond);
#define ZSTD_pthread_cond_wait(a, b)    pthread_cond_wait(*(a), *(b))
#define ZSTD_pthread_cond_signal(a)     pthread_cond_signal(*(a))
#define ZSTD_pthread_cond_broadcast(a)  pthread_cond_broadcast(*(a))

#define ZSTD_pthread_t                  pthread_t
#define ZSTD_pthread_create(a, b, c, d) pthread_create((a), (b), (c), (d))
#define ZSTD_pthread_join(a)         pthread_join((a),NULL)

#endif

#else

typedef int ZSTD_pthread_mutex_t;
#define ZSTD_pthread_mutex_init(a, b)   ((void)(a), (void)(b), 0)
#define ZSTD_pthread_mutex_destroy(a)   ((void)(a))
#define ZSTD_pthread_mutex_lock(a)      ((void)(a))
#define ZSTD_pthread_mutex_unlock(a)    ((void)(a))

typedef int ZSTD_pthread_cond_t;
#define ZSTD_pthread_cond_init(a, b)    ((void)(a), (void)(b), 0)
#define ZSTD_pthread_cond_destroy(a)    ((void)(a))
#define ZSTD_pthread_cond_wait(a, b)    ((void)(a), (void)(b))
#define ZSTD_pthread_cond_signal(a)     ((void)(a))
#define ZSTD_pthread_cond_broadcast(a)  ((void)(a))


#endif


#endif
