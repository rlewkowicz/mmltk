/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ipc_IPDLStructMember_h
#define mozilla_ipc_IPDLStructMember_h

#include <type_traits>
#include <utility>
#include "mozilla/Attributes.h"

namespace mozilla::ipc {

template <typename T>
struct IPDLStructMemberWrapper {
  template <typename... Args>
  constexpr MOZ_IMPLICIT IPDLStructMemberWrapper(Args&&... aArgs)
      : mVal(std::forward<Args>(aArgs)...) {}
  MOZ_IMPLICIT operator T&() { return mVal; }
  MOZ_IMPLICIT operator const T&() const { return mVal; }
  T mVal{};
};

template <typename T>
using IPDLStructMember =
    std::conditional_t<std::is_trivially_default_constructible_v<T>,
                       IPDLStructMemberWrapper<T>, T>;

}  

#endif  // mozilla_ipc_IPDLStructMember_h
