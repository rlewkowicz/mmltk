// Copyright 2021 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_STRINGS_CORD_ANALYSIS_H_
#define ABSL_STRINGS_CORD_ANALYSIS_H_

#include <cstddef>
#include <cstdint>

#include "absl/base/config.h"
#include "absl/base/nullability.h"
#include "absl/strings/internal/cord_internal.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace cord_internal {

size_t GetEstimatedMemoryUsage(const CordRep* absl_nonnull rep);

size_t GetMorePreciseMemoryUsage(const CordRep* absl_nonnull rep);

size_t GetEstimatedFairShareMemoryUsage(const CordRep* absl_nonnull rep);

}  
ABSL_NAMESPACE_END
}  


#endif  // ABSL_STRINGS_CORD_ANALYSIS_H_
