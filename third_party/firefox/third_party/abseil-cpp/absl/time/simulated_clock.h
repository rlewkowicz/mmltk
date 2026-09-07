// Copyright 2026 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef ABSL_TIME_SIMULATED_CLOCK_H_
#define ABSL_TIME_SIMULATED_CLOCK_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>

#include "absl/base/config.h"
#include "absl/base/macros.h"
#include "absl/base/nullability.h"
#include "absl/base/thread_annotations.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/clock.h"
#include "absl/time/clock_interface.h"
#include "absl/time/time.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class SimulatedClock : public Clock {
 public:
  explicit SimulatedClock(absl::Time t);
  SimulatedClock() : SimulatedClock(absl::UnixEpoch()) {}

  ~SimulatedClock() override;

  absl::Time TimeNow() override;

  void Sleep(absl::Duration d) override;

  void SleepUntil(absl::Time wakeup_time) override;

  int64_t SetTime(absl::Time t);

  int64_t AdvanceTime(absl::Duration d);

  bool AwaitWithDeadline(absl::Mutex* absl_nonnull mu,
                         const absl::Condition& cond,
                         absl::Time deadline) override
      ABSL_SHARED_LOCKS_REQUIRED(mu);

  std::optional<absl::Time> GetEarliestWakeupTime() const;

 private:
  template <class T>
  int64_t UpdateTime(const T& now_updater) ABSL_LOCKS_EXCLUDED(lock_);

  class WakeUpInfo;
  using WaiterList = std::multimap<absl::Time, std::shared_ptr<WakeUpInfo>>;

  mutable absl::Mutex lock_;
  absl::Time now_ ABSL_GUARDED_BY(lock_);
  WaiterList waiters_ ABSL_GUARDED_BY(lock_);
  int64_t num_await_calls_ ABSL_GUARDED_BY(lock_) = 0;
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_TIME_SIMULATED_CLOCK_H_
