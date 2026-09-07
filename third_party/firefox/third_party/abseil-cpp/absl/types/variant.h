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

#ifndef ABSL_TYPES_VARIANT_H_
#define ABSL_TYPES_VARIANT_H_

#include <stddef.h>

#include <variant>

#include "absl/base/config.h"
#include "absl/base/macros.h"
#include "absl/utility/utility.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

using bad_variant_access ABSL_REFACTOR_INLINE
    = std::bad_variant_access;

template <size_t I, typename... Args>
constexpr auto get(Args&&... args)
    -> decltype(std::get<I>(std::forward<Args>(args)...)) {
  return std::get<I>(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
constexpr decltype(std::get<T>(std::declval<Args>()...)) get(
    Args&&... args) {
  return std::get<T>(std::forward<Args>(args)...);
}

template <size_t I, typename... Args>
[[deprecated]] constexpr decltype(std::get_if<I>(std::declval<Args>()...))
get_if(Args&&... args) {
  return std::get_if<I>(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
[[deprecated]] constexpr decltype(std::get_if<T>(std::declval<Args>()...))
get_if(Args&&... args) {
  return std::get_if<T>(std::forward<Args>(args)...);
}

template <typename T, typename... Args>
constexpr decltype(std::holds_alternative<T>(
    std::declval<Args>()...))
holds_alternative(Args&&... args) {
  return std::holds_alternative<T>(std::forward<Args>(args)...);
}

using monostate ABSL_REFACTOR_INLINE
    = std::monostate;

template <typename... Types>
using variant ABSL_REFACTOR_INLINE
    = std::variant<Types...>;

template <size_t I, typename T>
using variant_alternative ABSL_REFACTOR_INLINE
    = std::variant_alternative<I, T>;

template <size_t I, typename T>
using variant_alternative_t ABSL_REFACTOR_INLINE
    = std::variant_alternative_t<I, T>;

inline constexpr size_t variant_npos ABSL_REFACTOR_INLINE
    = std::variant_npos;

template <typename T>
using variant_size ABSL_REFACTOR_INLINE
    = std::variant_size<T>;

template <typename T>
inline constexpr size_t variant_size_v ABSL_REFACTOR_INLINE
    = std::variant_size_v<T>;

template <typename... Args>
[[deprecated]] constexpr decltype(std::visit(std::declval<Args>()...)) visit(
    Args&&... args) {
  return std::visit(std::forward<Args>(args)...);
}

namespace variant_internal {
template <typename To>
struct ConversionVisitor {
  template <typename T>
  To operator()(T&& v) const {
    return To(std::forward<T>(v));
  }
};
}  

template <typename To, typename Variant>
To ConvertVariantTo(Variant&& variant) {
  return std::visit(variant_internal::ConversionVisitor<To>{},
                    std::forward<Variant>(variant));
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_TYPES_VARIANT_H_
