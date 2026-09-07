/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "nsCycleCollector.h"
#include "nsDebug.h"
#include "CycleCollectorStats.h"
#include "MainThreadUtils.h"
#include "mozilla/TimeStamp.h"

using namespace mozilla;

mozilla::CycleCollectorStats::CycleCollectorStats() {
  char* env = getenv("MOZ_CCTIMER");
  if (!env) {
    return;
  }
  if (strcmp(env, "none") == 0) {
    mFile = nullptr;
  } else if (strcmp(env, "stdout") == 0) {
    mFile = stdout;
  } else if (strcmp(env, "stderr") == 0) {
    mFile = stderr;
  } else {
    mFile = fopen(env, "a");
    if (!mFile) {
      NS_WARNING("Failed to open MOZ_CCTIMER log file.");
    }
  }
}

void mozilla::CycleCollectorStats::Clear() {
  if (mFile && mFile != stdout && mFile != stderr) {
    fclose(mFile);
  }
  *this = CycleCollectorStats();
}

MOZ_ALWAYS_INLINE
static TimeDuration TimeBetween(TimeStamp aStart, TimeStamp aEnd) {
  MOZ_ASSERT(aEnd >= aStart);
  return aEnd - aStart;
}

static TimeDuration TimeUntilNow(TimeStamp start) {
  if (start.IsNull()) {
    return TimeDuration();
  }
  return TimeBetween(start, TimeStamp::Now());
}

void mozilla::CycleCollectorStats::AfterCycleCollectionSlice() {
  MOZ_ASSERT(NS_IsMainThread());

  if (mBeginSliceTime.IsNull()) {
    return;
  }

  mEndSliceTime = TimeStamp::Now();


  TimeDuration sliceTime = TimeBetween(mBeginSliceTime, mEndSliceTime);
  mMaxSliceTime = std::max(mMaxSliceTime, sliceTime);
  mMaxSliceTimeSinceClear = std::max(mMaxSliceTimeSinceClear, sliceTime);
  mTotalSliceTime += sliceTime;
  mBeginSliceTime = TimeStamp();
}

void mozilla::CycleCollectorStats::PrepareForCycleCollection(TimeStamp aNow) {
  mBeginTime = aNow;
  mSuspected = nsCycleCollector_suspectedCount();
}

void mozilla::CycleCollectorStats::AfterPrepareForCycleCollectionSlice(
    TimeStamp aDeadline, TimeStamp aBeginTime, TimeStamp aMaybeAfterGCTime) {
  mBeginSliceTime = aBeginTime;
  mIdleDeadline = aDeadline;

  if (!aMaybeAfterGCTime.IsNull()) {
    mAnyLockedOut = true;
    mMaxGCDuration = std::max(mMaxGCDuration, aMaybeAfterGCTime - aBeginTime);
  }
}

void mozilla::CycleCollectorStats::AfterSyncForgetSkippable(
    TimeStamp beginTime) {
  mMaxSkippableDuration =
      std::max(mMaxSkippableDuration, TimeUntilNow(beginTime));
  mRanSyncForgetSkippable = true;
}

void mozilla::CycleCollectorStats::AfterForgetSkippable(
    mozilla::TimeStamp aStartTime, mozilla::TimeStamp aEndTime,
    uint32_t aRemovedPurples, bool aInIdle) {
  mozilla::TimeDuration duration = aEndTime - aStartTime;
  if (!mMinForgetSkippableTime || mMinForgetSkippableTime > duration) {
    mMinForgetSkippableTime = duration;
  }
  if (!mMaxForgetSkippableTime || mMaxForgetSkippableTime < duration) {
    mMaxForgetSkippableTime = duration;
  }
  mTotalForgetSkippableTime += duration;
  ++mForgetSkippableBeforeCC;

  mRemovedPurples += aRemovedPurples;

}
