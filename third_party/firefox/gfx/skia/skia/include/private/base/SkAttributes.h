/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkAttributes_DEFINED)
#define SkAttributes_DEFINED

#include "include/private/base/SkFeatures.h" // IWYU pragma: keep
#include "include/private/base/SkLoadUserConfig.h" // IWYU pragma: keep

#if defined(__clang__) || defined(__GNUC__)
#  define SK_ATTRIBUTE(attr) __attribute__((attr))
#else
#  define SK_ATTRIBUTE(attr)
#endif

#if !defined(SK_ALWAYS_INLINE)
#if defined(SK_BUILD_FOR_WIN)
#    define SK_ALWAYS_INLINE __forceinline
#else
#    define SK_ALWAYS_INLINE SK_ATTRIBUTE(always_inline) inline
#endif
#endif

#if !defined(SK_NEVER_INLINE)
#if defined(SK_BUILD_FOR_WIN)
#    define SK_NEVER_INLINE __declspec(noinline)
#else
#    define SK_NEVER_INLINE SK_ATTRIBUTE(noinline)
#endif
#endif

#if !defined(SK_PRINTF_LIKE)
#  define SK_PRINTF_LIKE(A, B) SK_ATTRIBUTE(format(printf, (A), (B)))
#endif

#if !defined(SK_NO_SANITIZE)
  #if defined(__has_attribute)
    #if __has_attribute(no_sanitize)
      #define SK_NO_SANITIZE(A) SK_ATTRIBUTE(no_sanitize(A))
    #else
      #define SK_NO_SANITIZE(A)
    #endif
  #else
    #define SK_NO_SANITIZE(A)
  #endif
#endif

#if defined(__clang__)
  #define SK_NO_SANITIZE_CFI SK_NO_SANITIZE("cfi")
#else
  #define SK_NO_SANITIZE_CFI
#endif

#if !defined(SK_TRIVIAL_ABI)
#  define SK_TRIVIAL_ABI
#endif

#if defined(__clang__)
  #define SK_REINITIALIZES [[clang::reinitializes]]
#else
  #define SK_REINITIALIZES
#endif

#endif
