// Copyright 2022 Google LLC
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT.

#ifndef THIRD_PARTY_UTF8_RANGE_UTF8_VALIDITY_H_
#define THIRD_PARTY_UTF8_RANGE_UTF8_VALIDITY_H_

#include <cstddef>

#include "absl/strings/string_view.h"
#include "utf8_range.h"

namespace utf8_range {

inline bool IsStructurallyValid(absl::string_view str) {
  return utf8_range_IsValid(str.data(), str.size());
}

inline size_t SpanStructurallyValid(absl::string_view str) {
  return utf8_range_ValidPrefix(str.data(), str.size());
}

}  

#endif  // THIRD_PARTY_UTF8_RANGE_UTF8_VALIDITY_H_
