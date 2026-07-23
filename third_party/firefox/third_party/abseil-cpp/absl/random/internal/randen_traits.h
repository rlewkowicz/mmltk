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

#ifndef ABSL_RANDOM_INTERNAL_RANDEN_TRAITS_H_
#define ABSL_RANDOM_INTERNAL_RANDEN_TRAITS_H_


#include <cstddef>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {


struct RandenTraits {
  static constexpr size_t kStateBytes = 256;  

  static constexpr size_t kCapacityBytes = 16;  

  static constexpr size_t kSeedBytes = kStateBytes - kCapacityBytes;

  static constexpr size_t kFeistelBlocks = 16;

  static constexpr size_t kFeistelRounds = 16 + 1;

  static constexpr size_t kKeyBytes = 16 * kFeistelRounds * kFeistelBlocks / 2;
};

extern const unsigned char kRandenRoundKeys[RandenTraits::kKeyBytes];
extern const unsigned char kRandenRoundKeysBE[RandenTraits::kKeyBytes];

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_INTERNAL_RANDEN_TRAITS_H_
