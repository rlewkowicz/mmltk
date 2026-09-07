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

#if !defined(ABSL_SYNCHRONIZATION_INTERNAL_KERNEL_TIMEOUT_H_)
#define ABSL_SYNCHRONIZATION_INTERNAL_KERNEL_TIMEOUT_H_

#include <sys/types.h>

#include <algorithm>
#include <chrono>  // NOLINT(build/c++11)
#include <cstdint>
#include <ctime>
#include <limits>

#include "absl/base/config.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace synchronization_internal {

class KernelTimeout {
 public:
  explicit KernelTimeout(absl::Time t);

  explicit KernelTimeout(absl::Duration d);

  constexpr KernelTimeout() : rep_(kNoTimeout) {}

  static constexpr KernelTimeout Never() { return KernelTimeout(); }

  bool has_timeout() const { return rep_ != kNoTimeout; }

  bool is_absolute_timeout() const { return (rep_ & 1) == 0; }

  bool is_relative_timeout() const { return (rep_ & 1) == 1; }

  struct timespec MakeAbsTimespec() const;

  struct timespec MakeRelativeTimespec() const;

  struct timespec MakeClockAbsoluteTimespec(clockid_t c) const;

  int64_t MakeAbsNanos() const;

  typedef unsigned long DWord;  // NOLINT
  DWord InMillisecondsFromNow() const;

  std::chrono::time_point<std::chrono::system_clock> ToChronoTimePoint() const;

  std::chrono::nanoseconds ToChronoDuration() const;

  static constexpr bool SupportsSteadyClock() { return true; }

 private:
  static int64_t SteadyClockNow();

  uint64_t rep_;

  int64_t RawAbsNanos() const { return static_cast<int64_t>(rep_ >> 1); }

  int64_t InNanosecondsFromNow() const;

  static constexpr uint64_t kNoTimeout = (std::numeric_limits<uint64_t>::max)();

  static constexpr int64_t kMaxNanos = (std::numeric_limits<int64_t>::max)();
};

}  
ABSL_NAMESPACE_END
}  

#endif
