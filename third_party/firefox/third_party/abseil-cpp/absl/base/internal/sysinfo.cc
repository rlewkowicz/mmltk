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

#include "absl/base/internal/sysinfo.h"

#include "absl/base/attributes.h"

#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif




#if defined(__myriad2__)
#include <rtems.h>
#endif

#if defined(__Fuchsia__)
#include <zircon/process.h>
#endif

#include <string.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <limits>
#include <thread>  // NOLINT(build/c++11)
#include <utility>
#include <vector>

#include "absl/base/call_once.h"
#include "absl/base/config.h"
#include "absl/base/internal/raw_logging.h"
#include "absl/base/internal/spinlock.h"
#include "absl/base/internal/unscaledcycleclock.h"
#include "absl/base/thread_annotations.h"

namespace absl {
ABSL_NAMESPACE_BEGIN
namespace base_internal {

namespace {


}  

static int GetNumCPUs() {
#if defined(__myriad2__)
  return 1;
#else
  return static_cast<int>(std::thread::hardware_concurrency());
#endif
}

#if defined(CTL_HW) && defined(HW_CPU_FREQ)

static double GetNominalCPUFrequency() {
  unsigned freq;
  size_t size = sizeof(freq);
  int mib[2] = {CTL_HW, HW_CPU_FREQ};
  if (sysctl(mib, 2, &freq, &size, nullptr, 0) == 0) {
    return static_cast<double>(freq);
  }
  return 1.0;
}

#else

static bool ReadLongFromFile(const char *file, long *value) {
  bool ret = false;
#if defined(_POSIX_C_SOURCE)
  const int file_mode = (O_RDONLY | O_CLOEXEC);
#else
  const int file_mode = O_RDONLY;
#endif

  int fd = open(file, file_mode);
  if (fd != -1) {
    char line[1024];
    char *err;
    memset(line, '\0', sizeof(line));
    ssize_t len;
    do {
      len = read(fd, line, sizeof(line) - 1);
    } while (len < 0 && errno == EINTR);
    if (len <= 0) {
      ret = false;
    } else {
      const long temp_value = strtol(line, &err, 10);
      if (line[0] != '\0' && (*err == '\n' || *err == '\0')) {
        *value = temp_value;
        ret = true;
      }
    }
    close(fd);
  }
  return ret;
}

#if defined(ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY)

static int64_t ReadMonotonicClockNanos() {
  struct timespec t;
#if defined(CLOCK_MONOTONIC_RAW)
  int rc = clock_gettime(CLOCK_MONOTONIC_RAW, &t);
#else
  int rc = clock_gettime(CLOCK_MONOTONIC, &t);
#endif
  if (rc != 0) {
    ABSL_RAW_LOG(FATAL, "clock_gettime() failed: (%d)", errno);
  }
  return int64_t{t.tv_sec} * 1000000000 + t.tv_nsec;
}

class UnscaledCycleClockWrapperForInitializeFrequency {
 public:
  static int64_t Now() { return base_internal::UnscaledCycleClock::Now(); }
};

struct TimeTscPair {
  int64_t time;  
  int64_t tsc;   
};

static TimeTscPair GetTimeTscPair() {
  int64_t best_latency = std::numeric_limits<int64_t>::max();
  TimeTscPair best;
  for (int i = 0; i < 10; ++i) {
    int64_t t0 = ReadMonotonicClockNanos();
    int64_t tsc = UnscaledCycleClockWrapperForInitializeFrequency::Now();
    int64_t t1 = ReadMonotonicClockNanos();
    int64_t latency = t1 - t0;
    if (latency < best_latency) {
      best_latency = latency;
      best.time = t0;
      best.tsc = tsc;
    }
  }
  return best;
}

static double MeasureTscFrequencyWithSleep(int sleep_nanoseconds) {
  auto t0 = GetTimeTscPair();
  struct timespec ts;
  ts.tv_sec = 0;
  ts.tv_nsec = sleep_nanoseconds;
  while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
  auto t1 = GetTimeTscPair();
  double elapsed_ticks = t1.tsc - t0.tsc;
  double elapsed_time = (t1.time - t0.time) * 1e-9;
  return elapsed_ticks / elapsed_time;
}

static double MeasureTscFrequency() {
  double last_measurement = -1.0;
  int sleep_nanoseconds = 1000000;  
  for (int i = 0; i < 8; ++i) {
    double measurement = MeasureTscFrequencyWithSleep(sleep_nanoseconds);
    if (measurement * 0.99 < last_measurement &&
        last_measurement < measurement * 1.01) {
      return measurement;
    }
    last_measurement = measurement;
    sleep_nanoseconds *= 2;
  }
  return last_measurement;
}

#endif

static double GetNominalCPUFrequency() {
  long freq = 0;

  if (ReadLongFromFile("/sys/devices/system/cpu/cpu0/tsc_freq_khz", &freq)) {
    return freq * 1e3;  
  }

#if defined(ABSL_INTERNAL_UNSCALED_CYCLECLOCK_FREQUENCY_IS_CPU_FREQUENCY)
  return MeasureTscFrequency();
#else

  if (ReadLongFromFile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq",
                       &freq)) {
    return freq * 1e3;  
  }

  return 1.0;
#endif
}

#endif

ABSL_CONST_INIT static once_flag init_num_cpus_once;
ABSL_CONST_INIT static int num_cpus = 0;

int NumCPUs() {
  base_internal::LowLevelCallOnce(
      &init_num_cpus_once, []() { num_cpus = GetNumCPUs(); });
  return num_cpus;
}

ABSL_CONST_INIT static once_flag init_nominal_cpu_frequency_once;
ABSL_CONST_INIT static double nominal_cpu_frequency = 1.0;

double NominalCPUFrequency() {
  base_internal::LowLevelCallOnce(
      &init_nominal_cpu_frequency_once,
      []() { nominal_cpu_frequency = GetNominalCPUFrequency(); });
  return nominal_cpu_frequency;
}

#if defined(__linux__)

#if !defined(SYS_gettid)
#define SYS_gettid __NR_gettid
#endif

pid_t GetTID() {
  return static_cast<pid_t>(syscall(SYS_gettid));
}

#elif defined(__akaros__)

pid_t GetTID() {
  if (in_vcore_context())
    return 0;
  return reinterpret_cast<struct pthread_tcb *>(current_uthread)->id;
}

#elif defined(__myriad2__)

pid_t GetTID() {
  uint32_t tid;
  rtems_task_ident(RTEMS_SELF, 0, &tid);
  return tid;
}

#elif defined(__Fuchsia__)

pid_t GetTID() {
  return static_cast<pid_t>(zx_thread_self());
}

#else

pid_t GetTID() {
  return static_cast<pid_t>(pthread_self());
}

#endif

pid_t GetCachedTID() {
#if defined(ABSL_HAVE_THREAD_LOCAL)
  static thread_local pid_t thread_id = GetTID();
  return thread_id;
#else
  return GetTID();
#endif
}

}  
ABSL_NAMESPACE_END
}  
