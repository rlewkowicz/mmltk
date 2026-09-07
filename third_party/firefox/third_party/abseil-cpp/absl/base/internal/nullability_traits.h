// Copyright 2025 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_INTERNAL_NULLABILITY_TRAITS_H_
#define ABSL_BASE_INTERNAL_NULLABILITY_TRAITS_H_

#include <type_traits>

#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

#if defined(__clang__) && !defined(__OBJC__) && \
    ABSL_HAVE_FEATURE(nullability_on_classes)
template <class T, class = void>
struct IsNullabilityCompatibleType {
  constexpr static bool value = false;
};

template <class T>
struct IsNullabilityCompatibleType<T, std::void_t<absl_nullable T>> {
  constexpr static bool value = true;
};
#else
template <class T, class = void>
struct IsNullabilityCompatibleType {
  constexpr static bool value = false;
};
#endif

template <typename T, bool ShouldAdd = IsNullabilityCompatibleType<T>::value>
struct AddNonnullIfCompatible;

template <typename T>
struct AddNonnullIfCompatible<T, false> {
  using type = T;
};
template <typename T>
struct AddNonnullIfCompatible<T, true> {
  using type = absl_nonnull T;
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_INTERNAL_NULLABILITY_TRAITS_H_
