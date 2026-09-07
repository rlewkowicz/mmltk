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

#ifndef ABSL_STRINGS_ESCAPING_H_
#define ABSL_STRINGS_ESCAPING_H_

#include <cstddef>
#include <string>
#include <vector>

#include "absl/base/attributes.h"
#include "absl/base/macros.h"
#include "absl/base/nullability.h"
#include "absl/strings/ascii.h"
#include "absl/strings/str_join.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

bool CUnescape(absl::string_view source, std::string* absl_nonnull dest,
               std::string* absl_nullable error);

inline bool CUnescape(absl::string_view source,
                      std::string* absl_nonnull dest) {
  return CUnescape(source, dest, nullptr);
}

std::string CEscape(absl::string_view src);

std::string CHexEscape(absl::string_view src);

std::string Utf8SafeCEscape(absl::string_view src);

std::string Utf8SafeCHexEscape(absl::string_view src);

std::string Base64Escape(absl::string_view src);
ABSL_REFACTOR_INLINE inline void
Base64Escape(absl::string_view src, std::string* absl_nonnull dest) {
  *dest = Base64Escape(src);
}

std::string WebSafeBase64Escape(absl::string_view src);
ABSL_REFACTOR_INLINE inline void
WebSafeBase64Escape(absl::string_view src, std::string* absl_nonnull dest) {
  *dest = WebSafeBase64Escape(src);
}

bool Base64Unescape(absl::string_view src, std::string* absl_nonnull dest);

bool WebSafeBase64Unescape(absl::string_view src,
                           std::string* absl_nonnull dest);

[[nodiscard]] bool HexStringToBytes(absl::string_view hex,
                                    std::string* absl_nonnull bytes);

ABSL_DEPRECATED("Use the HexStringToBytes() that returns a bool")
std::string HexStringToBytes(absl::string_view from);

std::string BytesToHexString(absl::string_view from);

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_ESCAPING_H_
