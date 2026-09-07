/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layout_ScrollAnimationPhysics_h_
#define mozilla_layout_ScrollAnimationPhysics_h_

#include "Units.h"
#include "mozilla/TimeStamp.h"
#include "nsPoint.h"

namespace mozilla {

class ScrollAnimationPhysics {
 public:
  virtual void Update(const TimeStamp& aTime, const nsPoint& aDestination,
                      const nsSize& aCurrentVelocity) = 0;

  virtual void ApplyContentShift(const CSSPoint& aShiftDelta) = 0;

  virtual nsSize VelocityAt(const TimeStamp& aTime) = 0;

  virtual nsPoint PositionAt(const TimeStamp& aTime) = 0;

  virtual bool IsFinished(const TimeStamp& aTime) = 0;

  virtual ~ScrollAnimationPhysics() = default;
};

static inline double ComputeAcceleratedWheelDelta(double aDelta,
                                                  int32_t aCounter,
                                                  int32_t aFactor) {
  if (!aDelta) {
    return aDelta;
  }
  return (aDelta * aCounter * double(aFactor) / 10);
}

}  

#endif  // mozilla_layout_ScrollAnimationPhysics_h_
