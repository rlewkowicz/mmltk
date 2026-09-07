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

#if !defined(ABSL_BASE_INTERNAL_UNSCALEDCYCLECLOCK_H_)
#define ABSL_BASE_INTERNAL_UNSCALEDCYCLECLOCK_H_

#include <cstdint>


#include "absl/base/config.h"
#include "absl/base/internal/unscaledcycleclock_config.h"

#if ABSL_USE_UNSCALED_CYCLECLOCK

namespace gloop_do_not_use {
class UnscaledCycleClockWrapperForPerCpuTest;
}  

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace time_internal {
class UnscaledCycleClockWrapperForGetCurrentTime;
}  

namespace base_internal {
class CycleClock;
class UnscaledCycleClockWrapperForInitializeFrequency;

class UnscaledCycleClock {
 private:
  UnscaledCycleClock() = delete;

  static int64_t Now();

  static double Frequency();

  friend class base_internal::CycleClock;
  friend class time_internal::UnscaledCycleClockWrapperForGetCurrentTime;
  friend class base_internal::UnscaledCycleClockWrapperForInitializeFrequency;
  friend class gloop_do_not_use::UnscaledCycleClockWrapperForPerCpuTest;
};

#if defined(__x86_64__)

inline int64_t UnscaledCycleClock::Now() {
  uint64_t low, high;
  __asm__ volatile("rdtsc" : "=a"(low), "=d"(high));
  return static_cast<int64_t>((high << 32) | low);
}

#elif defined(__aarch64__)

inline int64_t UnscaledCycleClock::Now() {
  int64_t virtual_timer_value;
  asm volatile("mrs %0, cntvct_el0" : "=r"(virtual_timer_value));
  return virtual_timer_value;
}

#endif

}  
ABSL_NAMESPACE_END
}  

#endif

#endif
