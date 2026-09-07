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

#ifndef ABSL_STRINGS_INTERNAL_STRINGIFY_STREAM_H_
#define ABSL_STRINGS_INTERNAL_STRINGIFY_STREAM_H_


#include <cstddef>
#include <ostream>
#include <string>
#include <type_traits>
#include <utility>

#include "absl/base/config.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

namespace strings_internal {

class StringifyStream {
 public:
  explicit StringifyStream(std::ostream& os) : sink_{os} {}

  template <typename T>
  friend StringifyStream& operator<<(StringifyStream& stream, const T& t) {
    if constexpr (HasStreamInsertion<T>::value) {
      stream.sink_.os << t;
    } else {
      AbslStringify(stream.sink_, t);
    }
    return stream;
  }

  template <typename T>
  friend StringifyStream& operator<<(StringifyStream&& stream, const T& t) {
    return stream << t;
  }

  StringifyStream& operator<<(std::ostream& (*func)(std::ostream&)) {
    sink_.os << func;
    return *this;
  }

 private:
  struct Sink {
    std::ostream& os;
    void Append(size_t count, char ch) { os << std::string(count, ch); }
    void Append(absl::string_view v) { os << v; }
    friend void AbslFormatFlush(Sink* sink, absl::string_view v) {
      sink->Append(v);
    }
  } sink_;

  template <typename T, typename = void>
  struct HasStreamInsertion : std::false_type {};

  template <typename T>
  struct HasStreamInsertion<T,
                            std::void_t<decltype(std::declval<std::ostream&>()
                                                 << std::declval<const T&>())>>
      : std::true_type {};
};

}  

ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_STRINGIFY_STREAM_H_
