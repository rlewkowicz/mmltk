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


#include "threading.h"

int g_ZSTD_threading_useless_symbol;


#if defined(ZSTD_MULTITHREAD) && DEBUGLEVEL >= 1 && !0

#define ZSTD_DEPS_NEED_MALLOC
#include "zstd_deps.h"

int ZSTD_pthread_mutex_init(ZSTD_pthread_mutex_t* mutex, pthread_mutexattr_t const* attr)
{
    assert(mutex != NULL);
    *mutex = (pthread_mutex_t*)ZSTD_malloc(sizeof(pthread_mutex_t));
    if (!*mutex)
        return 1;
    return pthread_mutex_init(*mutex, attr);
}

int ZSTD_pthread_mutex_destroy(ZSTD_pthread_mutex_t* mutex)
{
    assert(mutex != NULL);
    if (!*mutex)
        return 0;
    {
        int const ret = pthread_mutex_destroy(*mutex);
        ZSTD_free(*mutex);
        return ret;
    }
}

int ZSTD_pthread_cond_init(ZSTD_pthread_cond_t* cond, pthread_condattr_t const* attr)
{
    assert(cond != NULL);
    *cond = (pthread_cond_t*)ZSTD_malloc(sizeof(pthread_cond_t));
    if (!*cond)
        return 1;
    return pthread_cond_init(*cond, attr);
}

int ZSTD_pthread_cond_destroy(ZSTD_pthread_cond_t* cond)
{
    assert(cond != NULL);
    if (!*cond)
        return 0;
    {
        int const ret = pthread_cond_destroy(*cond);
        ZSTD_free(*cond);
        return ret;
    }
}

#endif
