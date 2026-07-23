/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_VelocityTracker_h
#define mozilla_layers_VelocityTracker_h

#include <utility>

#include "Axis.h"
#include "mozilla/Attributes.h"
#include "nsTArray.h"

namespace mozilla {
namespace layers {

class SimpleVelocityTracker : public VelocityTracker {
 public:
  explicit SimpleVelocityTracker(Axis* aAxis);
  void StartTracking(ParentLayerCoord aPos, TimeStamp aTimestamp) override;
  Maybe<float> AddPosition(ParentLayerCoord aPos,
                           TimeStamp aTimestamp) override;
  Maybe<float> ComputeVelocity(TimeStamp aTimestamp) override;
  void Clear() override;

 private:
  void AddVelocityToQueue(TimeStamp aTimestamp, float aVelocity);
  float ApplyFlingCurveToVelocity(float aVelocity) const;

  Axis* MOZ_NON_OWNING_REF mAxis;

  nsTArray<std::pair<TimeStamp, float>> mVelocityQueue;

  TimeStamp mVelocitySampleTime;
  ParentLayerCoord mVelocitySamplePos;
};

}  
}  

#endif
