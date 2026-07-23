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

#ifndef ABSL_RANDOM_INTERNAL_RANDEN_H_
#define ABSL_RANDOM_INTERNAL_RANDEN_H_

#include <cstddef>

#include "absl/random/internal/platform.h"
#include "absl/random/internal/randen_hwaes.h"
#include "absl/random/internal/randen_slow.h"
#include "absl/random/internal/randen_traits.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace random_internal {

class Randen {
 public:
  static constexpr size_t kStateBytes = RandenTraits::kStateBytes;
  static constexpr size_t kCapacityBytes = RandenTraits::kCapacityBytes;
  static constexpr size_t kSeedBytes = RandenTraits::kSeedBytes;

  ~Randen() = default;

  Randen();

  inline void Generate(void* state) const {
#if ABSL_RANDOM_INTERNAL_AES_DISPATCH
    if (has_crypto_) {
      RandenHwAes::Generate(keys_, state);
    } else {
      RandenSlow::Generate(keys_, state);
    }
#elif ABSL_HAVE_ACCELERATED_AES
    RandenHwAes::Generate(keys_, state);
#else
    RandenSlow::Generate(keys_, state);
#endif
  }

  inline void Absorb(const void* seed, void* state) const {
#if ABSL_RANDOM_INTERNAL_AES_DISPATCH
    if (has_crypto_) {
      RandenHwAes::Absorb(seed, state);
    } else {
      RandenSlow::Absorb(seed, state);
    }
#elif ABSL_HAVE_ACCELERATED_AES
    RandenHwAes::Absorb(seed, state);
#else
    RandenSlow::Absorb(seed, state);
#endif
  }

 private:
  const void* keys_;
#if ABSL_RANDOM_INTERNAL_AES_DISPATCH
  bool has_crypto_;
#endif
};

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_INTERNAL_RANDEN_H_
