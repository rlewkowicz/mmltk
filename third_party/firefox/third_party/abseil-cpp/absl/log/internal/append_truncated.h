// Copyright 2022 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_LOG_INTERNAL_APPEND_TRUNCATED_H_
#define ABSL_LOG_INTERNAL_APPEND_TRUNCATED_H_

#include <cstddef>
#include <cstring>
#include <string_view>

#include "absl/base/config.h"
#include "absl/strings/internal/utf8.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace log_internal {
inline size_t AppendTruncated(absl::string_view src, absl::Span<char> &dst) {
  if (src.size() > dst.size()) src = src.substr(0, dst.size());
  memcpy(dst.data(), src.data(), src.size());
  dst.remove_prefix(src.size());
  return src.size();
}
inline size_t AppendTruncated(std::wstring_view src, absl::Span<char> &dst) {
  absl::strings_internal::ShiftState state;
  size_t total_bytes_written = 0;
  for (const wchar_t wc : src) {
    if (dst.size() < absl::strings_internal::kMaxEncodedUTF8Size) break;
    size_t bytes_written =
        absl::strings_internal::WideToUtf8(wc, dst.data(), state);
    if (bytes_written == static_cast<size_t>(-1)) {
      constexpr wchar_t kReplacementCharacter = L'\uFFFD';
      bytes_written = absl::strings_internal::WideToUtf8(kReplacementCharacter,
                                                         dst.data(), state);
    }
    dst.remove_prefix(bytes_written);
    total_bytes_written += bytes_written;
  }
  return total_bytes_written;
}
inline size_t AppendTruncated(char c, size_t n, absl::Span<char> &dst) {
  if (n > dst.size()) n = dst.size();
  memset(dst.data(), c, n);
  dst.remove_prefix(n);
  return n;
}
}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_LOG_INTERNAL_APPEND_TRUNCATED_H_
