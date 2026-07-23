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

#ifndef ABSL_RANDOM_SEED_SEQUENCES_H_
#define ABSL_RANDOM_SEED_SEQUENCES_H_

#include <iterator>
#include <random>

#include "absl/base/config.h"
#include "absl/base/nullability.h"
#include "absl/random/internal/salted_seed_seq.h"
#include "absl/random/internal/seed_material.h"
#include "absl/random/seed_gen_exception.h"
#include "absl/strings/string_view.h"
#include "absl/types/span.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

using SeedSeq = random_internal::SaltedSeedSeq<std::seed_seq>;

template <typename URBG>
SeedSeq CreateSeedSeqFrom(URBG* urbg) {
  SeedSeq::result_type seed_material[random_internal::kEntropyBlocksNeeded];

  if (!random_internal::ReadSeedMaterialFromURBG(
          urbg, absl::MakeSpan(seed_material))) {
    random_internal::ThrowSeedGenException();
  }
  return SeedSeq(std::begin(seed_material), std::end(seed_material));
}

SeedSeq MakeSeedSeq();

ABSL_NAMESPACE_END
}  

#endif  // ABSL_RANDOM_SEED_SEQUENCES_H_
