/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_Likely_h
#define mozilla_Likely_h

#if defined(__clang__) || defined(__GNUC__)
#  define MOZ_LIKELY(x) (__builtin_expect(!!(x), 1))
#  define MOZ_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
#  define MOZ_LIKELY(x) (!!(x))
#  define MOZ_UNLIKELY(x) (!!(x))
#endif

#endif /* mozilla_Likely_h */
