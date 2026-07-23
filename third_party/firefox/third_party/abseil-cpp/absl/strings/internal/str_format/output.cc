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

#include "absl/strings/internal/str_format/output.h"

#include <errno.h>
#include <cstring>

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace str_format_internal {

namespace {
struct ClearErrnoGuard {
  ClearErrnoGuard() : old_value(errno) { errno = 0; }
  ~ClearErrnoGuard() {
    if (!errno) errno = old_value;
  }
  int old_value;
};
}  

void BufferRawSink::Write(string_view v) {
  size_t to_write = std::min(v.size(), size_);
  if (to_write > 0) {
    std::memcpy(buffer_, v.data(), to_write);
    buffer_ += to_write;
    size_ -= to_write;
  }
  total_written_ += v.size();
}

void FILERawSink::Write(string_view v) {
  while (!v.empty() && !error_) {
    ClearErrnoGuard guard;

    if (size_t result = std::fwrite(v.data(), 1, v.size(), output_)) {
      count_ += result;
      v.remove_prefix(result);
    } else {
      if (errno == EINTR) {
        continue;
      } else if (errno) {
        error_ = errno;
      } else if (std::ferror(output_)) {
        error_ = EBADF;
      } else {
        continue;
      }
    }
  }
}

}  
ABSL_NAMESPACE_END
}  
