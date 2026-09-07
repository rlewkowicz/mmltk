// Copyright 2020 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "absl/strings/internal/escaping.h"

#include <limits>

#include "absl/base/internal/endian.h"
#include "absl/base/internal/raw_logging.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace strings_internal {

size_t CalculateBase64EscapedLenInternal(size_t input_len, bool do_padding) {
  constexpr size_t kMaxSize = (std::numeric_limits<size_t>::max() - 1) / 4 * 3;
  ABSL_INTERNAL_CHECK(input_len <= kMaxSize,
                      "CalculateBase64EscapedLenInternal() overflow");
  size_t len = (input_len / 3) * 4;

  if (input_len % 3 == 0) {
  } else if (input_len % 3 == 1) {
    len += 2;
    if (do_padding) {
      len += 2;
    }
  } else {  
    len += 3;
    if (do_padding) {
      len += 1;
    }
  }

  return len;
}

}  
ABSL_NAMESPACE_END
}  
