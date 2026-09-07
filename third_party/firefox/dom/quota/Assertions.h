/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_ASSERTIONS_H_
#define DOM_QUOTA_ASSERTIONS_H_

#include <cstdint>

#include "nsLiteralString.h"
#include "nsString.h"

namespace mozilla::dom::quota {

template <typename T>
void AssertNoOverflow(uint64_t aDest, T aArg);

template <typename T, typename U>
void AssertNoUnderflow(T aDest, U aArg,
                       const nsACString& context = EmptyCString());

bool ShouldReportUnderflow(const nsACString& aContext);

bool IsOnIOThread();

void AssertIsOnIOThread();

void DiagnosticAssertIsOnIOThread();

void AssertCurrentThreadOwnsQuotaMutex();

}  

#if defined(NIGHTLY_BUILD) || defined(DEBUG)
#  define QM_ASSERT_NO_UNDERFLOW(aDest, aArg) \
    mozilla::dom::quota::AssertNoUnderflow(   \
        aDest, aArg,                          \
        nsDependentCString(__func__) + "::"_ns + nsDependentCString(#aDest))
#  define QM_ASSERT_NO_UNDERFLOW_2(aDest, aArg, aFieldContext) \
    mozilla::dom::quota::AssertNoUnderflow(                    \
        aDest, aArg,                                           \
        nsDependentCString(__func__) + "::"_ns + nsAutoCString(aFieldContext))
#else
#  define QM_ASSERT_NO_UNDERFLOW(aDest, aArg) \
    mozilla::dom::quota::AssertNoUnderflow(aDest, aArg)
#  define QM_ASSERT_NO_UNDERFLOW_2(aDest, aArg, aFieldContext) \
    mozilla::dom::quota::AssertNoUnderflow(aDest, aArg)
#endif

#endif  // DOM_QUOTA_ASSERTIONS_H_
