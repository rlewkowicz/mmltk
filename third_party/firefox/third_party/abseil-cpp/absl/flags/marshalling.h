//  Copyright 2019 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#ifndef ABSL_FLAGS_MARSHALLING_H_
#define ABSL_FLAGS_MARSHALLING_H_

#include <optional>
#include <string>
#include <vector>

#include "absl/base/config.h"
#include "absl/numeric/int128.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

template <typename T>
inline bool ParseFlag(absl::string_view input, T* dst, std::string* error);
template <typename T>
inline std::string UnparseFlag(const T& v);

namespace flags_internal {

bool AbslParseFlag(absl::string_view, bool*, std::string*);
bool AbslParseFlag(absl::string_view, short*, std::string*);           // NOLINT
bool AbslParseFlag(absl::string_view, unsigned short*, std::string*);  // NOLINT
bool AbslParseFlag(absl::string_view, int*, std::string*);             // NOLINT
bool AbslParseFlag(absl::string_view, unsigned int*, std::string*);    // NOLINT
bool AbslParseFlag(absl::string_view, long*, std::string*);            // NOLINT
bool AbslParseFlag(absl::string_view, unsigned long*, std::string*);   // NOLINT
bool AbslParseFlag(absl::string_view, long long*, std::string*);       // NOLINT
bool AbslParseFlag(absl::string_view, unsigned long long*,             // NOLINT
                   std::string*);
bool AbslParseFlag(absl::string_view, absl::int128*, std::string*);    // NOLINT
bool AbslParseFlag(absl::string_view, absl::uint128*, std::string*);   // NOLINT
bool AbslParseFlag(absl::string_view, float*, std::string*);
bool AbslParseFlag(absl::string_view, double*, std::string*);
bool AbslParseFlag(absl::string_view, std::string*, std::string*);
bool AbslParseFlag(absl::string_view, std::vector<std::string>*, std::string*);

template <typename T>
bool AbslParseFlag(absl::string_view text, std::optional<T>* f,
                   std::string* err) {
  if (text.empty()) {
    *f = std::nullopt;
    return true;
  }
  T value;
  if (!absl::ParseFlag(text, &value, err)) return false;

  *f = std::move(value);
  return true;
}

template <typename T>
bool InvokeParseFlag(absl::string_view input, T* dst, std::string* err) {
  return AbslParseFlag(input, dst, err);  
}

std::string AbslUnparseFlag(absl::string_view v);
std::string AbslUnparseFlag(const std::vector<std::string>&);

template <typename T>
std::string AbslUnparseFlag(const std::optional<T>& f) {
  return f.has_value() ? absl::UnparseFlag(*f) : "";
}

template <typename T>
std::string Unparse(const T& v) {
  return AbslUnparseFlag(v);  
}

std::string Unparse(bool v);
std::string Unparse(short v);               // NOLINT
std::string Unparse(unsigned short v);      // NOLINT
std::string Unparse(int v);                 // NOLINT
std::string Unparse(unsigned int v);        // NOLINT
std::string Unparse(long v);                // NOLINT
std::string Unparse(unsigned long v);       // NOLINT
std::string Unparse(long long v);           // NOLINT
std::string Unparse(unsigned long long v);  // NOLINT
std::string Unparse(absl::int128 v);
std::string Unparse(absl::uint128 v);
std::string Unparse(float v);
std::string Unparse(double v);

}  

template <typename T>
inline bool ParseFlag(absl::string_view input, T* dst, std::string* error) {
  return flags_internal::InvokeParseFlag(input, dst, error);
}

template <typename T>
inline std::string UnparseFlag(const T& v) {
  return flags_internal::Unparse(v);
}

enum class LogSeverity : int;
bool AbslParseFlag(absl::string_view, absl::LogSeverity*, std::string*);
std::string AbslUnparseFlag(absl::LogSeverity);

ABSL_NAMESPACE_END
}  

#endif  // ABSL_FLAGS_MARSHALLING_H_
