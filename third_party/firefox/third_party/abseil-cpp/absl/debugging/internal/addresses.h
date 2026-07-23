// Copyright 2025 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_DEBUGGING_INTERNAL_ADDRESSES_H_
#define ABSL_DEBUGGING_INTERNAL_ADDRESSES_H_

#include <stdint.h>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

inline uintptr_t StripPointerMetadata(uintptr_t ptr) {
#if defined(__aarch64__)
  register uintptr_t x30 __asm__("x30") = ptr;
#define ABSL_XPACLRI_HINT "hint #0x7;"
  asm(ABSL_XPACLRI_HINT : "+r"(x30));  
#undef ABSL_XPACLRI_HINT
  return x30;
#else
  return ptr;
#endif
}

inline uintptr_t StripPointerMetadata(void* ptr) {
  return StripPointerMetadata(reinterpret_cast<uintptr_t>(ptr));
}

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_DEBUGGING_INTERNAL_ADDRESSES_H_
