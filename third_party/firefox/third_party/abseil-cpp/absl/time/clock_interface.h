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

#ifndef ABSL_TIME_CLOCK_INTERFACE_H_
#define ABSL_TIME_CLOCK_INTERFACE_H_

#include "absl/base/config.h"
#include "absl/base/macros.h"
#include "absl/base/nullability.h"
#include "absl/synchronization/mutex.h"
#include "absl/time/time.h"

namespace absl {
ABSL_NAMESPACE_BEGIN

class Clock {
 public:
  static Clock& GetRealClock();

  virtual ~Clock();

  virtual absl::Time TimeNow() = 0;

  virtual void Sleep(absl::Duration d) = 0;

  virtual void SleepUntil(absl::Time wakeup_time) = 0;

  virtual bool AwaitWithDeadline(absl::Mutex* absl_nonnull mu,
                                 const absl::Condition& cond,
                                 absl::Time deadline) = 0;
};

ABSL_NAMESPACE_END
}  

#endif  // ABSL_TIME_CLOCK_INTERFACE_H_
