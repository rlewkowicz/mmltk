/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#if !defined(DAV1D_COMMON_ATTRIBUTES_H)
#define DAV1D_COMMON_ATTRIBUTES_H

#include "config.h"

#include <stddef.h>
#include <assert.h>

#if !defined(__has_attribute)
#define __has_attribute(x) 0
#endif

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

#if defined(__GNUC__)
#define ATTR_ALIAS __attribute__((may_alias))
#if defined(__MINGW32__) && !defined(__clang__)
#define ATTR_FORMAT_PRINTF(fmt, attr) __attribute__((__format__(__gnu_printf__, fmt, attr)))
#else
#define ATTR_FORMAT_PRINTF(fmt, attr) __attribute__((__format__(__printf__, fmt, attr)))
#endif
#define COLD __attribute__((cold))
#else
#define ATTR_ALIAS
#define ATTR_FORMAT_PRINTF(fmt, attr)
#define COLD
#endif

#if ARCH_X86_64
#define ALIGN_64_VAL 64
#define ALIGN_32_VAL 32
#define ALIGN_16_VAL 16
#elif ARCH_AARCH64 || ARCH_ARM || ARCH_LOONGARCH || ARCH_PPC64LE || ARCH_X86_32
#define ALIGN_64_VAL 16
#define ALIGN_32_VAL 16
#define ALIGN_16_VAL 16
#else
#define ALIGN_64_VAL 8
#define ALIGN_32_VAL 8
#define ALIGN_16_VAL 8
#endif

#if defined(_MSC_VER)
#define ALIGN(ll, a) \
    __declspec(align(a)) ll
#else
#define ALIGN(line, align) \
    line __attribute__((aligned(align)))
#endif

#define ALIGN_STK_64(type, var, sz1d, sznd) \
    ALIGN(type var[sz1d]sznd, ALIGN_64_VAL)
#define ALIGN_STK_32(type, var, sz1d, sznd) \
    ALIGN(type var[sz1d]sznd, ALIGN_32_VAL)
#define ALIGN_STK_16(type, var, sz1d, sznd) \
    ALIGN(type var[sz1d]sznd, ALIGN_16_VAL)

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#elif __has_attribute(noclone)
#define NOINLINE __attribute__((noinline, noclone))
#else
#define NOINLINE __attribute__((noinline))
#endif

#if defined(_MSC_VER)
#define ALWAYS_INLINE __forceinline
#else
#define ALWAYS_INLINE __attribute__((always_inline)) inline
#endif

#if (defined(__ELF__) || defined(__MACH__) || (0 && defined(__clang__))) && __has_attribute(visibility)
#define EXTERN extern __attribute__((visibility("hidden")))
#else
#define EXTERN extern
#endif

#if ARCH_X86_64 && __has_attribute(model)
#define ATTR_MCMODEL_SMALL __attribute__((model("small")))
#else
#define ATTR_MCMODEL_SMALL
#endif

#if defined(__clang__)
#define NO_SANITIZE(x) __attribute__((no_sanitize(x)))
#else
#define NO_SANITIZE(x)
#endif

#if defined(NDEBUG) && (defined(__GNUC__) || defined(__clang__))
#undef assert
#define assert(x) do { if (!(x)) __builtin_unreachable(); } while (0)
#elif defined(NDEBUG) && defined(_MSC_VER)
#undef assert
#define assert __assume
#endif

#if defined(__GNUC__) && !defined(__INTEL_COMPILER) && !defined(__clang__)
#    define dav1d_uninit(x) x=x
#else
#    define dav1d_uninit(x) x
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>

static inline int ctz(const unsigned int mask) {
    unsigned long idx;
    _BitScanForward(&idx, mask);
    return idx;
}

static inline int clz(const unsigned int mask) {
    unsigned long leading_zero = 0;
    _BitScanReverse(&leading_zero, mask);
    return (31 - leading_zero);
}

static inline int clzll(const unsigned long long mask) {
    if (mask >> 32)
        return clz((unsigned)(mask >> 32));
    else
        return clz((unsigned)mask) + 32;
}
#else
static inline int ctz(const unsigned int mask) {
    return __builtin_ctz(mask);
}

static inline int clz(const unsigned int mask) {
    return __builtin_clz(mask);
}

static inline int clzll(const unsigned long long mask) {
    return __builtin_clzll(mask);
}
#endif

#if !defined(static_assert)
#define CHECK_OFFSET(type, field, name) \
    struct check_##type##_##field { int x[(name == offsetof(type, field)) ? 1 : -1]; }
#define CHECK_SIZE(type, size) \
    struct check_##type##_size { int x[(size == sizeof(type)) ? 1 : -1]; }
#else
#define CHECK_OFFSET(type, field, name) \
    static_assert(name == offsetof(type, field), #field)
#define CHECK_SIZE(type, size) \
    static_assert(size == sizeof(type), #type)
#endif

#if defined(_MSC_VER)
#define PACKED(...) __pragma(pack(push, 1)) __VA_ARGS__ __pragma(pack(pop))
#else
#define PACKED(...) __VA_ARGS__ __attribute__((__packed__))
#endif

#endif
