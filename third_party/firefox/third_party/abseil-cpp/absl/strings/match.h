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
#ifndef ABSL_STRINGS_MATCH_H_
#define ABSL_STRINGS_MATCH_H_

#include <cstring>

#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

inline bool StrContains(absl::string_view haystack,
                        absl::string_view needle) noexcept {
  return haystack.find(needle, 0) != haystack.npos;
}

inline bool StrContains(absl::string_view haystack, char needle) noexcept {
  return haystack.find(needle) != haystack.npos;
}

inline constexpr bool StartsWith(absl::string_view text,
                                 absl::string_view prefix) noexcept {
  if (prefix.empty()) {
    return true;
  }
  if (text.size() < prefix.size()) {
    return false;
  }
  absl::string_view possible_match = text.substr(0, prefix.size());

  return possible_match == prefix;
}

inline constexpr bool EndsWith(absl::string_view text,
                               absl::string_view suffix) noexcept {
  if (suffix.empty()) {
    return true;
  }
  if (text.size() < suffix.size()) {
    return false;
  }
  absl::string_view possible_match = text.substr(text.size() - suffix.size());
  return possible_match == suffix;
}
bool StrContainsIgnoreCase(absl::string_view haystack,
                           absl::string_view needle) noexcept;

bool StrContainsIgnoreCase(absl::string_view haystack,
                           char needle) noexcept;

bool EqualsIgnoreCase(absl::string_view piece1,
                      absl::string_view piece2) noexcept;

bool StartsWithIgnoreCase(absl::string_view text,
                          absl::string_view prefix) noexcept;

bool EndsWithIgnoreCase(absl::string_view text,
                        absl::string_view suffix) noexcept;

absl::string_view FindLongestCommonPrefix(absl::string_view a,
                                          absl::string_view b);

absl::string_view FindLongestCommonSuffix(absl::string_view a,
                                          absl::string_view b);

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_MATCH_H_
