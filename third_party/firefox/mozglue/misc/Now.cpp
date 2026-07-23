/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Now.h"


#include <stdint.h>

#include "mozilla/TimeStamp.h"
#include "mozilla/Maybe.h"

namespace mozilla {

#if defined(XP_UNIX)  // including BSDs and Android
#  include <time.h>

static constexpr uint64_t kNSperMS = 1000000;

static uint64_t TimespecToMilliseconds(struct timespec aTs) {
  return aTs.tv_sec * 1000 + aTs.tv_nsec / kNSperMS;
}

Maybe<uint64_t> NowExcludingSuspendMs() {
  struct timespec ts = {0};

  if (clock_gettime(CLOCK_MONOTONIC, &ts)) {
    return Nothing();
  }
  return Some(TimespecToMilliseconds(ts));
}

Maybe<uint64_t> NowIncludingSuspendMs() {
#if !defined(CLOCK_BOOTTIME)
  return Nothing();
#else
  struct timespec ts = {0};

  if (clock_gettime(CLOCK_BOOTTIME, &ts)) {
    return Nothing();
  }
  return Some(TimespecToMilliseconds(ts));
#endif
}

#else

Maybe<uint64_t> NowExcludingSuspendMs() { return Nothing(); }
Maybe<uint64_t> NowIncludingSuspendMs() { return Nothing(); }

#endif

}  
