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

#ifndef ABSL_RANDOM_RANDOM_H_
#define ABSL_RANDOM_RANDOM_H_

#include <cstdint>
#include <random>

#include "absl/base/config.h"
#include "absl/random/distributions.h"  // IWYU pragma: export
#include "absl/random/internal/nonsecure_base.h"
#include "absl/random/internal/pcg_engine.h"
#include "absl/random/internal/randen_engine.h"
#include "absl/random/seed_sequences.h"  // IWYU pragma: export

namespace absl {
ABSL_NAMESPACE_BEGIN

class BitGen : private random_internal::NonsecureURBGBase<
                   random_internal::randen_engine<uint64_t>> {
  using Base = random_internal::NonsecureURBGBase<
      random_internal::randen_engine<uint64_t>>;

 public:
  using result_type = typename Base::result_type;

  using Base::Base;
  using Base::operator=;

  using Base::min;

  using Base::max;

  using Base::discard;

  using Base::operator();

  using Base::operator==;
  using Base::operator!=;
};

class InsecureBitGen : private random_internal::NonsecureURBGBase<
                           random_internal::pcg64_2018_engine> {
  using Base =
      random_internal::NonsecureURBGBase<random_internal::pcg64_2018_engine>;

 public:
  using result_type = typename Base::result_type;

  using Base::Base;
  using Base::operator=;

  using Base::min;

  using Base::max;

  using Base::discard;

  using Base::operator();

  using Base::operator==;
  using Base::operator!=;
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_RANDOM_H_
