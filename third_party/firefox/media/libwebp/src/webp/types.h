// Copyright 2010 Google Inc. All Rights Reserved.
// Use of this source code is governed by a BSD-style license

#if !defined(WEBP_WEBP_TYPES_H_)
#define WEBP_WEBP_TYPES_H_

#include <stddef.h>  // IWYU pragma: export for size_t

#if !defined(_MSC_VER)
#include <inttypes.h>  // IWYU pragma: export
#if defined(__cplusplus) || !defined(__STRICT_ANSI__) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 199901L)
#define WEBP_INLINE inline
#else
#define WEBP_INLINE
#endif
#else
typedef signed   char int8_t;
typedef unsigned char uint8_t;
typedef signed   short int16_t;
typedef unsigned short uint16_t;
typedef signed   int int32_t;
typedef unsigned int uint32_t;
typedef unsigned long long int uint64_t;
typedef long long int int64_t;
#define WEBP_INLINE __forceinline
#endif

#if !defined(WEBP_NODISCARD)
#if defined(WEBP_ENABLE_NODISCARD) && WEBP_ENABLE_NODISCARD
#if (defined(__cplusplus) && __cplusplus >= 201703L) || \
    (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define WEBP_NODISCARD [[nodiscard]]
#else
#if defined(__clang__) && defined(__has_attribute)
#if __has_attribute(warn_unused_result)
#define WEBP_NODISCARD __attribute__((warn_unused_result))
#else
#define WEBP_NODISCARD
#endif
#else
#define WEBP_NODISCARD
#endif
#endif
           (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L) */
#else
#define WEBP_NODISCARD
#endif
#endif

#if !defined(WEBP_EXTERN)
#if defined(__GNUC__) && __GNUC__ >= 4
#  define WEBP_EXTERN extern __attribute__ ((visibility ("default")))
#else
#  define WEBP_EXTERN extern
#endif
#endif

#define WEBP_ABI_IS_INCOMPATIBLE(a, b) (((a) >> 8) != ((b) >> 8))

#if defined(__cplusplus)
extern "C" {
#endif

WEBP_NODISCARD WEBP_EXTERN void* WebPMalloc(size_t size);

WEBP_EXTERN void WebPFree(void* ptr);

#if defined(__cplusplus)
}    
#endif

#endif
