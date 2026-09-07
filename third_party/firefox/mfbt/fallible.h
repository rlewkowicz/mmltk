/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_fallible_h
#define mozilla_fallible_h

#if defined(__cplusplus)


#  include <new>

namespace mozilla {

using fallible_t = std::nothrow_t;

static const fallible_t& fallible = std::nothrow;

}  

#endif

#endif  // mozilla_fallible_h
