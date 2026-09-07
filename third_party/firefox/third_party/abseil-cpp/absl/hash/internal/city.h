// Copyright 2018 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_HASH_INTERNAL_CITY_H_
#define ABSL_HASH_INTERNAL_CITY_H_

#include <stdint.h>
#include <stdlib.h>  // for size_t.

#include <utility>

#include "absl/base/config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace hash_internal {

uint64_t CityHash64(const char *s, size_t len);

uint64_t CityHash64WithSeed(const char *s, size_t len, uint64_t seed);

uint64_t CityHash64WithSeeds(const char *s, size_t len, uint64_t seed0,
                             uint64_t seed1);

uint32_t CityHash32(const char *s, size_t len);

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_HASH_INTERNAL_CITY_H_
