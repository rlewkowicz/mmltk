/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_StaticAnalysisFunctions_h
#define mozilla_StaticAnalysisFunctions_h

#ifdef MOZ_CLANG_PLUGIN

#  ifndef __cplusplus
#    ifndef bool
#      include <stdbool.h>
#    endif
#    define MOZ_CONSTEXPR
#  else  // __cplusplus
#    include "mozilla/Attributes.h"
#    define MOZ_CONSTEXPR constexpr
#  endif

#  ifdef __cplusplus
template <typename T>
static MOZ_ALWAYS_INLINE T* MOZ_KnownLive(T* ptr) {
  return ptr;
}

template <typename T>
static MOZ_ALWAYS_INLINE T& MOZ_KnownLive(T& ref) {
  return ref;
}

#  endif

static MOZ_ALWAYS_INLINE MOZ_CONSTEXPR bool MOZ_AssertAssignmentTest(
    bool exprResult) {
  return exprResult;
}

#  define MOZ_CHECK_ASSERT_ASSIGNMENT(expr) MOZ_AssertAssignmentTest(!!(expr))

#else

#  define MOZ_CHECK_ASSERT_ASSIGNMENT(expr) (!!(expr))
#  define MOZ_KnownLive(expr) (expr)

#endif /* MOZ_CLANG_PLUGIN */
#endif /* StaticAnalysisFunctions_h */
