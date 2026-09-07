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

#ifndef ABSL_BASE_INTERNAL_SPINLOCK_WAIT_H_
#define ABSL_BASE_INTERNAL_SPINLOCK_WAIT_H_


#include <stdint.h>
#include <atomic>

#include "absl/base/internal/scheduling_mode.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

struct SpinLockWaitTransition {
  uint32_t from;
  uint32_t to;
  bool done;
};

uint32_t SpinLockWait(std::atomic<uint32_t> *w, int n,
                      const SpinLockWaitTransition trans[],
                      SchedulingMode scheduling_mode);

void SpinLockWake(std::atomic<uint32_t> *w, bool all);

void SpinLockDelay(std::atomic<uint32_t> *w, uint32_t value, int loop,
                   base_internal::SchedulingMode scheduling_mode);

int SpinLockSuggestedDelayNS(int loop);

}  
ABSL_NAMESPACE_END
}  

extern "C" {
void ABSL_INTERNAL_C_SYMBOL(AbslInternalSpinLockWake)(std::atomic<uint32_t> *w,
                                                      bool all);
void ABSL_INTERNAL_C_SYMBOL(AbslInternalSpinLockDelay)(
    std::atomic<uint32_t> *w, uint32_t value, int loop,
    absl::base_internal::SchedulingMode scheduling_mode);
}

inline void absl::base_internal::SpinLockWake(std::atomic<uint32_t> *w,
                                              bool all) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalSpinLockWake)(w, all);
}

inline void absl::base_internal::SpinLockDelay(
    std::atomic<uint32_t> *w, uint32_t value, int loop,
    absl::base_internal::SchedulingMode scheduling_mode) {
  ABSL_INTERNAL_C_SYMBOL(AbslInternalSpinLockDelay)
  (w, value, loop, scheduling_mode);
}

#endif  // ABSL_BASE_INTERNAL_SPINLOCK_WAIT_H_
