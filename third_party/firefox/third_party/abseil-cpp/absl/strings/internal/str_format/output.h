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

#ifndef ABSL_STRINGS_INTERNAL_STR_FORMAT_OUTPUT_H_
#define ABSL_STRINGS_INTERNAL_STR_FORMAT_OUTPUT_H_

#include <cstdio>
#include <ios>
#include <ostream>
#include <string>

#include "absl/base/port.h"
#include "absl/strings/string_view.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace str_format_internal {

class BufferRawSink {
 public:
  BufferRawSink(char* buffer, size_t size) : buffer_(buffer), size_(size) {}

  size_t total_written() const { return total_written_; }
  void Write(string_view v);

 private:
  char* buffer_;
  size_t size_;
  size_t total_written_ = 0;
};

class FILERawSink {
 public:
  explicit FILERawSink(std::FILE* output) : output_(output) {}

  void Write(string_view v);

  size_t count() const { return count_; }
  int error() const { return error_; }

 private:
  std::FILE* output_;
  int error_ = 0;
  size_t count_ = 0;
};

inline void AbslFormatFlush(std::string* out, string_view s) {
  out->append(s.data(), s.size());
}
inline void AbslFormatFlush(std::ostream* out, string_view s) {
  out->write(s.data(), static_cast<std::streamsize>(s.size()));
}

inline void AbslFormatFlush(FILERawSink* sink, string_view v) {
  sink->Write(v);
}

inline void AbslFormatFlush(BufferRawSink* sink, string_view v) {
  sink->Write(v);
}

template <typename T>
auto InvokeFlush(T* out, string_view s) -> decltype(AbslFormatFlush(out, s)) {
  AbslFormatFlush(out, s);
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_STR_FORMAT_OUTPUT_H_
