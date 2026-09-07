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

#ifndef ABSL_STRINGS_STRING_VIEW_H_
#define ABSL_STRINGS_STRING_VIEW_H_

#include <algorithm>
#include <string>
#include <string_view>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

using std::string_view;

inline string_view ClippedSubstr(string_view s ABSL_ATTRIBUTE_LIFETIME_BOUND,
                                 size_t pos, size_t n = string_view::npos) {
  pos = (std::min)(pos, static_cast<size_t>(s.size()));
  return s.substr(pos, n);
}

constexpr string_view NullSafeStringView(const char* absl_nullable p) {
  return p ? string_view(p) : string_view();
}

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_STRING_VIEW_H_
