/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under both the BSD-style license (found in the
 * LICENSE file in the root directory of this source tree) and the GPLv2 (found
 * in the COPYING file in the root directory of this source tree).
 * You may select, at your option, one of the above-listed licenses.
 */


#if !defined(ZSTD_DEPS_COMMON)
#define ZSTD_DEPS_COMMON

#if defined(__linux) || defined(__linux__) || defined(linux) || defined(__gnu_linux__) || \
    0 || defined(__MSYS__)
#if !defined(_GNU_SOURCE) && !0 /* NDK doesn't ship qsort_r(). */
#define _GNU_SOURCE
#endif
#endif

#include <limits.h>
#include <stddef.h>
#include <string.h>

#if defined(__GNUC__) && __GNUC__ >= 4
# define ZSTD_memcpy(d,s,l) __builtin_memcpy((d),(s),(l))
# define ZSTD_memmove(d,s,l) __builtin_memmove((d),(s),(l))
# define ZSTD_memset(p,v,l) __builtin_memset((p),(v),(l))
#else
# define ZSTD_memcpy(d,s,l) memcpy((d),(s),(l))
# define ZSTD_memmove(d,s,l) memmove((d),(s),(l))
# define ZSTD_memset(p,v,l) memset((p),(v),(l))
#endif

#endif

#if defined(ZSTD_DEPS_NEED_MALLOC)
#if !defined(ZSTD_DEPS_MALLOC)
#define ZSTD_DEPS_MALLOC

#include <stdlib.h>

#define ZSTD_malloc(s) malloc(s)
#define ZSTD_calloc(n,s) calloc((n), (s))
#define ZSTD_free(p) free((p))

#endif
#endif

#if defined(ZSTD_DEPS_NEED_MATH64)
#if !defined(ZSTD_DEPS_MATH64)
#define ZSTD_DEPS_MATH64

#define ZSTD_div64(dividend, divisor) ((dividend) / (divisor))

#endif
#endif

#if defined(ZSTD_DEPS_NEED_ASSERT)
#if !defined(ZSTD_DEPS_ASSERT)
#define ZSTD_DEPS_ASSERT

#include <assert.h>

#endif
#endif

#if defined(ZSTD_DEPS_NEED_IO)
#if !defined(ZSTD_DEPS_IO)
#define ZSTD_DEPS_IO

#include <stdio.h>
#define ZSTD_DEBUG_PRINT(...) fprintf(stderr, __VA_ARGS__)

#endif
#endif

#if defined(ZSTD_DEPS_NEED_STDINT)
#if !defined(ZSTD_DEPS_STDINT)
#define ZSTD_DEPS_STDINT

#include <stdint.h>

#endif
#endif
