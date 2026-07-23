// Copyright 2024 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_DEBUGGING_INTERNAL_DECODE_RUST_PUNYCODE_H_
#define ABSL_DEBUGGING_INTERNAL_DECODE_RUST_PUNYCODE_H_

#include "absl/base/config.h"
#include "absl/base/nullability.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace debugging_internal {

struct DecodeRustPunycodeOptions {
  const char* absl_nonnull punycode_begin;
  const char* absl_nonnull punycode_end;
  char* absl_nonnull out_begin;
  char* absl_nonnull out_end;
};

char* absl_nullable DecodeRustPunycode(DecodeRustPunycodeOptions options);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_DEBUGGING_INTERNAL_DECODE_RUST_PUNYCODE_H_
