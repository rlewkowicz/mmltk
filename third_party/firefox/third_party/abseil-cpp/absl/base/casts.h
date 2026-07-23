// Copyright 2017 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_CASTS_H_
#define ABSL_BASE_CASTS_H_

#include <cstring>
#include <memory>
#include <type_traits>
#include <typeinfo>
#include <utility>

#ifdef __has_include
#if __has_include(<version>)
#include <version>  // For __cpp_lib_bit_cast.
#endif
#endif

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L
#include <bit>  // For std::bit_cast.
#endif  // defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/macros.h"
#include "absl/base/optimization.h"
#include "absl/base/options.h"
#include "absl/meta/type_traits.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename To>
constexpr std::enable_if_t<
    !type_traits_internal::IsView<std::enable_if_t<
        !std::is_reference_v<To>, std::remove_cv_t<To>>>::value,
    To>
implicit_cast(absl::type_identity_t<To> to) {
  return to;
}
template <typename To>
constexpr std::enable_if_t<
    type_traits_internal::IsView<std::enable_if_t<!std::is_reference_v<To>,
                                                  std::remove_cv_t<To>>>::value,
    To>
implicit_cast(absl::type_identity_t<To> to ABSL_ATTRIBUTE_LIFETIME_BOUND) {
  return to;
}
template <typename To>
constexpr std::enable_if_t<std::is_reference_v<To>, To> implicit_cast(
    absl::type_identity_t<To> to ABSL_ATTRIBUTE_LIFETIME_BOUND) {
  return std::forward<absl::type_identity_t<To>>(to);
}

#if defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L

using std::bit_cast;

#else  // defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L

template <typename Dest, typename Source,
          std::enable_if_t<sizeof(Dest) == sizeof(Source) &&
                               std::is_trivially_copyable_v<Source> &&
                               std::is_trivially_copyable_v<Dest>
#if !ABSL_HAVE_BUILTIN(__builtin_bit_cast)
                               && std::is_default_constructible_v<Dest>
#endif  // !ABSL_HAVE_BUILTIN(__builtin_bit_cast)
                           ,
                           int> = 0>
#if ABSL_HAVE_BUILTIN(__builtin_bit_cast)
inline constexpr Dest bit_cast(const Source& source) {
  return __builtin_bit_cast(Dest, source);
}
#else  // ABSL_HAVE_BUILTIN(__builtin_bit_cast)
inline Dest bit_cast(const Source& source) {
  Dest dest;
  memcpy(static_cast<void*>(std::addressof(dest)),
         static_cast<const void*>(std::addressof(source)), sizeof(dest));
  return dest;
}
#endif  // ABSL_HAVE_BUILTIN(__builtin_bit_cast)

#endif  // defined(__cpp_lib_bit_cast) && __cpp_lib_bit_cast >= 201806L

namespace base_internal {

[[noreturn]] ABSL_ATTRIBUTE_NOINLINE void BadDownCastCrash(
    const char* source_type, const char* target_type);

template <typename To, typename From>
inline void ValidateDownCast(From* f ABSL_ATTRIBUTE_UNUSED) {
#ifdef ABSL_INTERNAL_HAS_RTTI
#if !defined(NDEBUG) || (ABSL_OPTION_HARDENED == 1)
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnonnull-compare"
#endif
  if (ABSL_PREDICT_FALSE(f != nullptr && dynamic_cast<To>(f) == nullptr)) {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
    absl::base_internal::BadDownCastCrash(
        typeid(*f).name(), typeid(std::remove_pointer_t<To>).name());
  }
#endif
#endif
}

}  


template <typename To, typename From>  
[[nodiscard]]
inline To down_cast(From* f) {  
  static_assert(std::is_pointer_v<To>, "target type not a pointer");
  if constexpr (!std::is_same_v<std::remove_cv_t<std::remove_pointer_t<To>>,
                                std::remove_cv_t<From>>) {
    static_assert(std::is_polymorphic_v<From>,
                  "source type must be polymorphic");
    static_assert(std::is_polymorphic_v<std::remove_pointer_t<To>>,
                  "target type must be polymorphic");
  }
  static_assert(
      std::is_convertible_v<std::remove_cv_t<std::remove_pointer_t<To>>*,
                            std::remove_cv_t<From>*>,
      "target type not derived from source type");

  absl::base_internal::ValidateDownCast<To>(f);

  return static_cast<To>(f);
}

template <typename To, typename From>
[[nodiscard]]
inline To down_cast(From& f) {
  static_assert(std::is_lvalue_reference_v<To>, "target type not a reference");
  if constexpr (!std::is_same_v<std::remove_cv_t<std::remove_reference_t<To>>,
                                std::remove_cv_t<From>>) {
    static_assert(std::is_polymorphic_v<From>,
                  "source type must be polymorphic");
    static_assert(std::is_polymorphic_v<std::remove_reference_t<To>>,
                  "target type must be polymorphic");
  }
  static_assert(
      std::is_convertible_v<std::remove_cv_t<std::remove_reference_t<To>>*,
                            std::remove_cv_t<From>*>,
      "target type not derived from source type");

  absl::base_internal::ValidateDownCast<std::remove_reference_t<To>*>(
      std::addressof(f));

  return static_cast<To>(f);
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_CASTS_H_
