/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "PotentialCheckerboardDurationTracker.h"


namespace mozilla {
namespace layers {

PotentialCheckerboardDurationTracker::PotentialCheckerboardDurationTracker()
    : mInCheckerboard(false), mInTransform(false) {}

void PotentialCheckerboardDurationTracker::CheckerboardSeen() {
  if (!Tracking()) {
    mCurrentPeriodStart = TimeStamp::Now();
  }
  mInCheckerboard = true;
}

void PotentialCheckerboardDurationTracker::CheckerboardDone(
    bool aRecordTelemetry) {
  MOZ_ASSERT(Tracking());
  mInCheckerboard = false;
  if (!Tracking()) {
    if (aRecordTelemetry) {

    }
  }
}

void PotentialCheckerboardDurationTracker::InTransform(bool aInTransform,
                                                       bool aRecordTelemetry) {
  if (aInTransform == mInTransform) {
    return;
  }

  if (!Tracking()) {
    mInTransform = aInTransform;
    mCurrentPeriodStart = TimeStamp::Now();
    return;
  }

  mInTransform = aInTransform;

  if (!Tracking()) {
    if (aRecordTelemetry) {

    }
  }
}

bool PotentialCheckerboardDurationTracker::Tracking() const {
  return mInTransform || mInCheckerboard;
}

}  
}  
