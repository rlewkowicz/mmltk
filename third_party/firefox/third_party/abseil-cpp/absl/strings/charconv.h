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

#ifndef ABSL_STRINGS_CHARCONV_H_
#define ABSL_STRINGS_CHARCONV_H_

#include <system_error>  // NOLINT(build/c++11)

#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

enum class chars_format {
  scientific = 1,
  fixed = 2,
  hex = 4,
  general = fixed | scientific,
};

struct from_chars_result {
  const char* absl_nonnull ptr;
  std::errc ec;
};

absl::from_chars_result from_chars(const char* absl_nonnull first,
                                   const char* absl_nonnull last,
                                   double& value,  // NOLINT
                                   chars_format fmt = chars_format::general);

absl::from_chars_result from_chars(const char* absl_nonnull first,
                                   const char* absl_nonnull last,
                                   float& value,  // NOLINT
                                   chars_format fmt = chars_format::general);

inline constexpr chars_format operator&(chars_format lhs, chars_format rhs) {
  return static_cast<chars_format>(static_cast<int>(lhs) &
                                   static_cast<int>(rhs));
}
inline constexpr chars_format operator|(chars_format lhs, chars_format rhs) {
  return static_cast<chars_format>(static_cast<int>(lhs) |
                                   static_cast<int>(rhs));
}
inline constexpr chars_format operator^(chars_format lhs, chars_format rhs) {
  return static_cast<chars_format>(static_cast<int>(lhs) ^
                                   static_cast<int>(rhs));
}
inline constexpr chars_format operator~(chars_format arg) {
  return static_cast<chars_format>(~static_cast<int>(arg));
}
inline chars_format& operator&=(chars_format& lhs, chars_format rhs) {
  lhs = lhs & rhs;
  return lhs;
}
inline chars_format& operator|=(chars_format& lhs, chars_format rhs) {
  lhs = lhs | rhs;
  return lhs;
}
inline chars_format& operator^=(chars_format& lhs, chars_format rhs) {
  lhs = lhs ^ rhs;
  return lhs;
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_CHARCONV_H_
