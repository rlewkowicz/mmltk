// Copyright 2022 The Abseil Authors
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_BASE_INTERNAL_CYCLECLOCK_CONFIG_H_
#define ABSL_BASE_INTERNAL_CYCLECLOCK_CONFIG_H_

#include <cstdint>

#include "absl/base/config.h"
#include "absl/base/internal/unscaledcycleclock_config.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

#if ABSL_USE_UNSCALED_CYCLECLOCK
#ifdef NDEBUG
#ifdef ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY
inline constexpr int32_t kCycleClockShift = 1;
#else
inline constexpr int32_t kCycleClockShift = 0;
#endif
#else   // NDEBUG
inline constexpr int32_t kCycleClockShift = 2;
#endif  // NDEBUG

inline constexpr double kCycleClockFrequencyScale =
    1.0 / (1 << kCycleClockShift);

#endif  // ABSL_USE_UNSCALED_CYCLECLOCK

}  
ABSL_NAMESPACE_END
}  

#endif  // ABSL_BASE_INTERNAL_CYCLECLOCK_CONFIG_H_
