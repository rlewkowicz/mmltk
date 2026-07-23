// Copyright 2026 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_TYPES_OPTIONAL_REF_H_
#define ABSL_TYPES_OPTIONAL_REF_H_

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/hardening.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename T>
class optional_ref {
  template <typename U>
  using EnableIfConvertibleFrom =
      std::enable_if_t<std::is_convertible_v<U*, T*>>;

 public:
  using value_type = T;

  constexpr optional_ref() : ptr_(nullptr) {}
  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      std::nullopt_t)
      : ptr_(nullptr) {}

  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      T& input ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : ptr_(std::addressof(input)) {}

  template <typename U, typename = EnableIfConvertibleFrom<const U>>
  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      const std::optional<U>& input ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : ptr_(input.has_value() ? std::addressof(*input) : nullptr) {}
  template <typename U, typename = EnableIfConvertibleFrom<U>>
  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      std::optional<U>& input ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : ptr_(input.has_value() ? std::addressof(*input) : nullptr) {}

  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      T* input ABSL_ATTRIBUTE_LIFETIME_BOUND)
      : ptr_(input) {}

  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      std::nullptr_t) = delete;

  optional_ref(const optional_ref<T>&) = default;
  optional_ref<T>& operator=(const optional_ref<T>&) = delete;

  template <typename U, typename = EnableIfConvertibleFrom<U>>
  constexpr optional_ref(  // NOLINT(google-explicit-constructor)
      optional_ref<U> input)
      : ptr_(input.as_pointer()) {}

  constexpr bool has_value() const { return ptr_ != nullptr; }

  constexpr T& value() const {
    return ABSL_PREDICT_TRUE(ptr_ != nullptr)
               ? *ptr_
               : ((void)std::optional<T>().value(), *ptr_);
  }

  template <typename U>
  constexpr T value_or(U&& default_value) const {
    if (false) {
      (void)std::add_const_t<std::optional<T>>{}.value_or(
          std::forward<U>(default_value));
    }
    return ptr_ != nullptr ? *ptr_
                           : static_cast<T>(std::forward<U>(default_value));
  }

  constexpr T& operator*() const {
    absl::base_internal::HardeningAssertNonNull(ptr_);
    return *ptr_;
  }
  constexpr T* operator->() const {
    absl::base_internal::HardeningAssertNonNull(ptr_);
    return ptr_;
  }

  constexpr T* as_pointer() const { return ptr_; }
  template <typename U = std::decay_t<T>>
  constexpr std::optional<U> as_optional() const {
    if (ptr_ == nullptr) return std::nullopt;
    return *ptr_;
  }

 private:
  T* const ptr_;

  static_assert(!std::is_same_v<std::nullopt_t, std::remove_cv_t<T>>,
                "optional_ref<nullopt_t> is not allowed.");
  static_assert(!std::is_same_v<std::in_place_t, std::remove_cv_t<T>>,
                "optional_ref<in_place_t> is not allowed.");
};


template <typename T>
optional_ref(const T&) -> optional_ref<const T>;
template <typename T>
optional_ref(T&) -> optional_ref<T>;

template <typename T>
optional_ref(const std::optional<T>&) -> optional_ref<const T>;
template <typename T>
optional_ref(std::optional<T>&) -> optional_ref<T>;

template <typename T>
optional_ref(T*) -> optional_ref<T>;

namespace optional_ref_internal {

template <typename T, typename U>
using enable_if_equality_comparable_t = std::enable_if_t<std::is_convertible_v<
    decltype(std::declval<T>() == std::declval<U>()), bool>>;

}  


template <typename T>
constexpr bool operator==(optional_ref<T> a, std::nullopt_t) {
  return !a.has_value();
}
template <typename T>
constexpr bool operator==(std::nullopt_t, optional_ref<T> b) {
  return !b.has_value();
}
template <typename T>
constexpr bool operator!=(optional_ref<T> a, std::nullopt_t) {
  return a.has_value();
}
template <typename T>
constexpr bool operator!=(std::nullopt_t, optional_ref<T> b) {
  return b.has_value();
}


template <typename T, typename U>
constexpr bool operator==(optional_ref<T> a, optional_ref<U> b) {
  return a.has_value() ? *a == b : !b.has_value();
}


template <
    typename T, typename U,
    typename = optional_ref_internal::enable_if_equality_comparable_t<T, U>>
constexpr bool operator==(const T& a, optional_ref<U> b) {
  return b.has_value() && a == *b;
}
template <
    typename T, typename U,
    typename = optional_ref_internal::enable_if_equality_comparable_t<T, U>>
constexpr bool operator==(optional_ref<T> a, const U& b) {
  return b == a;
}


template <typename T, typename U>
constexpr bool operator!=(optional_ref<T> a, optional_ref<U> b) {
  return !(a == b);
}
template <
    typename T, typename U,
    typename = optional_ref_internal::enable_if_equality_comparable_t<T, U>>
constexpr bool operator!=(optional_ref<T> a, const U& b) {
  return !(a == b);
}
template <
    typename T, typename U,
    typename = optional_ref_internal::enable_if_equality_comparable_t<T, U>>
constexpr bool operator!=(const T& a, optional_ref<U> b) {
  return !(a == b);
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_TYPES_OPTIONAL_REF_H_
