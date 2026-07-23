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

#include "absl/random/internal/randen.h"

#include "absl/base/internal/raw_logging.h"
#include "absl/random/internal/randen_detect.h"


namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {
namespace {

struct RandenState {
  const void* keys;
  bool has_crypto;
};

RandenState GetRandenState() {
  static const RandenState state = []() {
    RandenState tmp;
#if ABSL_RANDOM_INTERNAL_AES_DISPATCH
    if (HasRandenHwAesImplementation() && CPUSupportsRandenHwAes()) {
      tmp.has_crypto = true;
      tmp.keys = RandenHwAes::GetKeys();
    } else {
      tmp.has_crypto = false;
      tmp.keys = RandenSlow::GetKeys();
    }
#elif ABSL_HAVE_ACCELERATED_AES
    tmp.has_crypto = true;
    tmp.keys = RandenHwAes::GetKeys();
#else
    tmp.has_crypto = false;
    tmp.keys = RandenSlow::GetKeys();
#endif
    return tmp;
  }();
  return state;
}

}  

Randen::Randen() {
  auto tmp = GetRandenState();
  keys_ = tmp.keys;
#if ABSL_RANDOM_INTERNAL_AES_DISPATCH
  has_crypto_ = tmp.has_crypto;
#endif
}

}  
ABSL_NAMESPACE_END
}  
