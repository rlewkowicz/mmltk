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


#include "absl/base/internal/cycleclock.h"

#include <atomic>
#include <chrono>  // NOLINT(build/c++11)

#include "absl/base/attributes.h"
#include "absl/base/config.h"
#include "absl/base/internal/unscaledcycleclock.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

#if ABSL_USE_UNSCALED_CYCLECLOCK

ABSL_CONST_INIT std::atomic<CycleClockSourceFunc>
    CycleClock::cycle_clock_source_{nullptr};

void CycleClockSource::Register(CycleClockSourceFunc source) {
  CycleClock::cycle_clock_source_.store(source, std::memory_order_release);
}


#else

int64_t CycleClock::Now() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

double CycleClock::Frequency() {
  return 1e9;
}

#endif

}  
ABSL_NAMESPACE_END
}  
