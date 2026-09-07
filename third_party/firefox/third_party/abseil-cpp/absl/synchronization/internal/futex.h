// Copyright 2020 The Abseil Authors.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//      https://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#if !defined(ABSL_SYNCHRONIZATION_INTERNAL_FUTEX_H_)
#define ABSL_SYNCHRONIZATION_INTERNAL_FUTEX_H_

#include "absl/base/config.h"

#include <sys/time.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <time.h>

#include <atomic>
#include <cstdint>
#include <limits>

#include "absl/base/optimization.h"
#include "absl/synchronization/internal/kernel_timeout.h"

#if defined(ABSL_INTERNAL_HAVE_FUTEX)
#error ABSL_INTERNAL_HAVE_FUTEX may not be set on the command line
#elif defined(__BIONIC__)
#define ABSL_INTERNAL_HAVE_FUTEX
#elif defined(__linux__) && defined(FUTEX_CLOCK_REALTIME)
#define ABSL_INTERNAL_HAVE_FUTEX
#endif

#if defined(ABSL_INTERNAL_HAVE_FUTEX)

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace synchronization_internal {

#if defined(__BIONIC__)
#if !defined(SYS_futex)
#define SYS_futex __NR_futex
#endif
#if !defined(FUTEX_WAIT_BITSET)
#define FUTEX_WAIT_BITSET 9
#endif
#if !defined(FUTEX_PRIVATE_FLAG)
#define FUTEX_PRIVATE_FLAG 128
#endif
#if !defined(FUTEX_CLOCK_REALTIME)
#define FUTEX_CLOCK_REALTIME 256
#endif
#if !defined(FUTEX_BITSET_MATCH_ANY)
#define FUTEX_BITSET_MATCH_ANY 0xFFFFFFFF
#endif
#endif

#if defined(__NR_futex_time64) && !defined(SYS_futex_time64)
#define SYS_futex_time64 __NR_futex_time64
#endif

#if defined(SYS_futex_time64) && !defined(SYS_futex)
#define SYS_futex SYS_futex_time64
using FutexTimespec = struct timespec;
#else
struct FutexTimespec {
  long tv_sec;   // NOLINT
  long tv_nsec;  // NOLINT
};
#endif

class FutexImpl {
 public:
  static int Wait(std::atomic<int32_t>* v, int32_t val) {
    return WaitAbsoluteTimeout(v, val, nullptr);
  }

  static int WaitAbsoluteTimeout(std::atomic<int32_t>* v, int32_t val,
                                 const struct timespec* abs_timeout) {
    FutexTimespec ts;
    auto err = syscall(
        SYS_futex, reinterpret_cast<int32_t*>(v),
        FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME, val,
        ToFutexTimespec(abs_timeout, &ts), nullptr, FUTEX_BITSET_MATCH_ANY);
    if (err != 0) {
      return -errno;
    }
    return 0;
  }

  static int WaitRelativeTimeout(std::atomic<int32_t>* v, int32_t val,
                                 const struct timespec* rel_timeout) {
    FutexTimespec ts;
    auto err =
        syscall(SYS_futex, reinterpret_cast<int32_t*>(v), FUTEX_PRIVATE_FLAG,
                val, ToFutexTimespec(rel_timeout, &ts));
    if (err != 0) {
      return -errno;
    }
    return 0;
  }

  static int Wake(std::atomic<int32_t>* v, int32_t count) {
    auto err = syscall(SYS_futex, reinterpret_cast<int32_t*>(v),
                       FUTEX_WAKE | FUTEX_PRIVATE_FLAG, count);
    if (ABSL_PREDICT_FALSE(err < 0)) {
      return -errno;
    }
    return 0;
  }

 private:
  static FutexTimespec* ToFutexTimespec(const struct timespec* userspace_ts,
                                        FutexTimespec* futex_ts) {
    if (userspace_ts == nullptr) {
      return nullptr;
    }

    using FutexSeconds = decltype(futex_ts->tv_sec);
    using FutexNanoseconds = decltype(futex_ts->tv_nsec);

    constexpr auto kMaxSeconds{(std::numeric_limits<FutexSeconds>::max)()};
    if (userspace_ts->tv_sec > kMaxSeconds) {
      futex_ts->tv_sec = kMaxSeconds;
    } else {
      futex_ts->tv_sec = static_cast<FutexSeconds>(userspace_ts->tv_sec);
    }
    futex_ts->tv_nsec = static_cast<FutexNanoseconds>(userspace_ts->tv_nsec);
    return futex_ts;
  }
};

class Futex : public FutexImpl {};

}  
ABSL_NAMESPACE_END
}  

#endif

#endif
