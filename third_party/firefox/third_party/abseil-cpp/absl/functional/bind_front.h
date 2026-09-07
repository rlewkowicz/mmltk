// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_FUNCTIONAL_BIND_FRONT_H_
#define ABSL_FUNCTIONAL_BIND_FRONT_H_

#ifdef __has_include
#if __has_include(<version>)
#include <version>
#endif
#endif

#if defined(__cpp_lib_bind_front) && __cpp_lib_bind_front >= 201907L
#include <functional>  // For std::bind_front.
#endif  // defined(__cpp_lib_bind_front) && __cpp_lib_bind_front >= 201907L

#include <utility>

#include "absl/functional/internal/front_binder.h"
#include "absl/utility/utility.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

#if defined(__cpp_lib_bind_front) && __cpp_lib_bind_front >= 201907L
using std::bind_front;
#else   // defined(__cpp_lib_bind_front) && __cpp_lib_bind_front >= 201907L
template <class F, class... BoundArgs>
constexpr functional_internal::bind_front_t<F, BoundArgs...> bind_front(
    F&& func, BoundArgs&&... args) {
  return functional_internal::bind_front_t<F, BoundArgs...>(
      std::in_place, std::forward<F>(func), std::forward<BoundArgs>(args)...);
}
#endif  // defined(__cpp_lib_bind_front) && __cpp_lib_bind_front >= 201907L

ABSL_NAMESPACE_END
}  

#endif  // ABSL_FUNCTIONAL_BIND_FRONT_H_
