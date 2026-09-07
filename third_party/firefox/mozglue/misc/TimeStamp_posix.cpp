/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <string.h>



#  define KINFO_PROC struct kinfo_proc

#  define KP_START_SEC p_ustart_sec
#  define KP_START_USEC p_ustart_usec

#include "mozilla/Sprintf.h"
#include "mozilla/TimeStamp.h"

#if !defined(__wasi__)
#  include <pthread.h>
#endif

#if defined(CLOCK_MONOTONIC_COARSE)
static bool sSupportsMonotonicCoarseClock = false;
#endif

#if !defined(__wasi__)
static const uint16_t kNsPerUs = 1000;
#endif

static const uint64_t kNsPerSec = 1000000000;
static const double kNsPerMsd = 1000000.0;
static const double kNsPerSecd = 1000000000.0;

static uint64_t TimespecToNs(const struct timespec& aTs) {
  uint64_t baseNs = uint64_t(aTs.tv_sec) * kNsPerSec;
  return baseNs + uint64_t(aTs.tv_nsec);
}

static uint64_t ClockTimeNs(const clockid_t aClockId = CLOCK_MONOTONIC) {
  struct timespec ts;
#if defined(CLOCK_MONOTONIC_COARSE)
  MOZ_RELEASE_ASSERT(
      aClockId == CLOCK_MONOTONIC ||
      (sSupportsMonotonicCoarseClock && aClockId == CLOCK_MONOTONIC_COARSE));
#else
  MOZ_RELEASE_ASSERT(aClockId == CLOCK_MONOTONIC);
#endif
  clock_gettime(aClockId, &ts);

  return TimespecToNs(ts);
}

namespace mozilla {

double BaseTimeDurationPlatformUtils::ToSeconds(int64_t aTicks) {
  return double(aTicks) / kNsPerSecd;
}

int64_t BaseTimeDurationPlatformUtils::TicksFromMilliseconds(
    double aMilliseconds) {
  double result = aMilliseconds * kNsPerMsd;
  if (result >= double(INT64_MAX)) {
    return INT64_MAX;
  }
  if (result <= INT64_MIN) {
    return INT64_MIN;
  }

  return result;
}

static bool gInitialized = false;

void TimeStamp::Startup() {
  if (gInitialized) {
    return;
  }

  struct timespec dummy;
  if (clock_gettime(CLOCK_MONOTONIC, &dummy) != 0) {
    MOZ_CRASH("CLOCK_MONOTONIC is absent!");
  }

#if defined(CLOCK_MONOTONIC_COARSE)
  if (clock_gettime(CLOCK_MONOTONIC_COARSE, &dummy) == 0) {
    sSupportsMonotonicCoarseClock = true;
  }
#endif

  gInitialized = true;
}

void TimeStamp::Shutdown() {}

TimeStamp TimeStamp::Now(bool aHighResolution) {
#if defined(CLOCK_MONOTONIC_COARSE)
  if (!aHighResolution && sSupportsMonotonicCoarseClock) {
    return TimeStamp(ClockTimeNs(CLOCK_MONOTONIC_COARSE));
  }
#endif
  return TimeStamp(ClockTimeNs(CLOCK_MONOTONIC));
}

#if defined(XP_LINUX) || 0


static uint64_t JiffiesSinceBoot(const char* aFile) {
  char stat[512];

  FILE* f = fopen(aFile, "r");
  if (!f) {
    return 0;
  }

  int n = fread(&stat, 1, sizeof(stat) - 1, f);

  fclose(f);

  if (n <= 0) {
    return 0;
  }

  stat[n] = 0;

  long long unsigned startTime = 0;  
  char* s = strrchr(stat, ')');

  if (!s) {
    return 0;
  }

  int rv = sscanf(s + 2,
                  "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u "
                  "%*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                  &startTime);

  if (rv != 1 || !startTime) {
    return 0;
  }

  return startTime;
}


static void* ComputeProcessUptimeThread(void* aTime) {
  uint64_t* uptime = static_cast<uint64_t*>(aTime);
  long hz = sysconf(_SC_CLK_TCK);

  *uptime = 0;

  if (!hz) {
    return nullptr;
  }

  char threadStat[40];
  SprintfLiteral(threadStat, "/proc/self/task/%d/stat",
                 (pid_t)syscall(__NR_gettid));

  uint64_t threadJiffies = JiffiesSinceBoot(threadStat);
  uint64_t selfJiffies = JiffiesSinceBoot("/proc/self/stat");

  if (!threadJiffies || !selfJiffies) {
    return nullptr;
  }

  *uptime = ((threadJiffies - selfJiffies) * kNsPerSec) / hz;
  return nullptr;
}


uint64_t TimeStamp::ComputeProcessUptime() {
  uint64_t uptime = 0;
  pthread_t uptime_pthread;

  if (pthread_create(&uptime_pthread, nullptr, ComputeProcessUptimeThread,
                     &uptime)) {
    MOZ_CRASH("Failed to create process uptime thread.");
    return 0;
  }

  pthread_join(uptime_pthread, nullptr);

  return uptime / kNsPerUs;
}

#else

uint64_t TimeStamp::ComputeProcessUptime() { return 0; }

#endif

}  
