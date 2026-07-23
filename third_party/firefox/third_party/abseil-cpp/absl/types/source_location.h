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

#ifndef ABSL_TYPES_SOURCE_LOCATION_H_
#define ABSL_TYPES_SOURCE_LOCATION_H_

#include <cstdint>
#include <type_traits>

#include "absl/base/config.h"
#include "absl/base/nullability.h"

#ifdef ABSL_HAVE_STD_SOURCE_LOCATION
#include <source_location>  // NOLINT(build/c++20)
#endif

#if defined(ABSL_USES_STD_SOURCE_LOCATION) && \
    defined(ABSL_HAVE_STD_SOURCE_LOCATION)
namespace absl {
ABSL_NAMESPACE_BEGIN
using SourceLocation = std::source_location;
ABSL_NAMESPACE_END
}  

#else  // ABSL_HAVE_STD_SOURCE_LOCATION

namespace absl {
ABSL_NAMESPACE_BEGIN

class SourceLocation {
  struct PrivateTag {
   private:
    explicit PrivateTag() = default;
    friend class SourceLocation;
  };

 public:
  SourceLocation() = default;

#ifdef ABSL_HAVE_STD_SOURCE_LOCATION
  constexpr SourceLocation(  // NOLINT(google-explicit-constructor)
      std::source_location loc)
      : SourceLocation(loc.line(), loc.file_name()) {}
#endif

#ifdef ABSL_INTERNAL_HAVE_BUILTIN_LINE_FILE
  static constexpr SourceLocation current(
      PrivateTag = PrivateTag{}, std::uint_least32_t line = __builtin_LINE(),
      const char* absl_nonnull file_name = __builtin_FILE()) {
    return SourceLocation(line, file_name);
  }
#else
  static constexpr SourceLocation current() {
    return SourceLocation(1, "<source_location>");
  }
#endif
  constexpr std::uint_least32_t line() const noexcept { return line_; }

  constexpr std::uint_least32_t column() const noexcept { return 0; }

  constexpr const char* absl_nonnull file_name() const noexcept {
    return file_name_;
  }

  constexpr const char* absl_nonnull function_name() const noexcept {
    return "";
  }

 private:
  constexpr SourceLocation(std::uint_least32_t line,
                           const char* absl_nonnull file_name)
      : line_(line), file_name_(file_name) {}

  friend constexpr int UseUnused() {
    static_assert(SourceLocation(0, nullptr).unused_column_ == 0,
                  "Use the otherwise-unused member.");
    return 0;
  }

  std::uint_least32_t line_ = 0;
  std::uint_least32_t unused_column_ = 0;
  const char* absl_nonnull file_name_ = "";
};

ABSL_NAMESPACE_END
}  
#endif  // ABSL_HAVE_STD_SOURCE_LOCATION

#endif  // ABSL_TYPES_SOURCE_LOCATION_H_
