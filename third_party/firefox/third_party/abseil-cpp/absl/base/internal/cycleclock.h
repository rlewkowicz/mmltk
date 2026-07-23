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


#if !defined(ABSL_BASE_INTERNAL_CYCLECLOCK_H_)
#define ABSL_BASE_INTERNAL_CYCLECLOCK_H_

#include <atomic>
#include <cstdint>

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/cycleclock_config.h"
#include "absl/base/internal/unscaledcycleclock.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

using CycleClockSourceFunc = int64_t (*)();

class CycleClock {
 public:
  static int64_t Now();

  static double Frequency();

 private:
#if ABSL_USE_UNSCALED_CYCLECLOCK
  static CycleClockSourceFunc LoadCycleClockSource();

  static constexpr int32_t kShift = kCycleClockShift;
  static constexpr double kFrequencyScale = kCycleClockFrequencyScale;

  ABSL_CONST_INIT static std::atomic<CycleClockSourceFunc> cycle_clock_source_;
#endif

  CycleClock() = delete;  
  CycleClock(const CycleClock&) = delete;
  CycleClock& operator=(const CycleClock&) = delete;

  friend class CycleClockSource;
};

class CycleClockSource {
 private:
  static void Register(CycleClockSourceFunc source);
};

#if ABSL_USE_UNSCALED_CYCLECLOCK

inline CycleClockSourceFunc CycleClock::LoadCycleClockSource() {
#if !defined(__x86_64__)
  if (cycle_clock_source_.load(std::memory_order_relaxed) == nullptr) {
    return nullptr;
  }
#endif

  return cycle_clock_source_.load(std::memory_order_acquire);
}

inline int64_t CycleClock::Now() {
  auto fn = LoadCycleClockSource();
  if (fn == nullptr) {
    return base_internal::UnscaledCycleClock::Now() >> kShift;
  }
  return fn() >> kShift;
}

inline double CycleClock::Frequency() {
  return kFrequencyScale * base_internal::UnscaledCycleClock::Frequency();
}

#endif

}  
ABSL_NAMESPACE_END
}  

#endif
