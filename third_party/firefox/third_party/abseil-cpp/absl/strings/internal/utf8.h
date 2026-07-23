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

#ifndef ABSL_STRINGS_INTERNAL_UTF8_H_
#define ABSL_STRINGS_INTERNAL_UTF8_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace strings_internal {

enum { kMaxEncodedUTF8Size = 4 };
size_t EncodeUTF8Char(char* buffer, char32_t utf8_char);

struct ShiftState {
  bool saw_high_surrogate = false;
  unsigned char bits = 0;
};

size_t WideToUtf8(wchar_t wc, char* buf, ShiftState& s);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_STRINGS_INTERNAL_UTF8_H_
