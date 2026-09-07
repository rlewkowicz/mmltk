/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_MoveOnlyFunction_h
#define mozilla_MoveOnlyFunction_h

#define FU2_WITH_LIMITED_EMPTY_PROPAGATION

#include "function2/function2.hpp"

namespace mozilla {

template <typename Signature>
using MoveOnlyFunction = fu2::function_base<
     true,
     false,
     fu2::capacity_fixed<2 * sizeof(void*), alignof(void*)>,
     false,
     false,
     Signature>;

}  

#endif  // mozilla_MoveOnlyFunction_h
