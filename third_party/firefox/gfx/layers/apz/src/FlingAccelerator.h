/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_FlingAccelerator_h
#define mozilla_layers_FlingAccelerator_h

#include "mozilla/layers/SampleTime.h"
#include "Units.h"

namespace mozilla {
namespace layers {

struct FlingHandoffState;

class FlingAccelerator final {
 public:
  FlingAccelerator() = default;

  void Reset();

  bool IsTracking() const { return mIsTracking; }

  ParentLayerPoint GetFlingStartingVelocity(
      const SampleTime& aNow, const ParentLayerPoint& aVelocity,
      const FlingHandoffState& aHandoffState);

  void ObserveFlingCanceled(const ParentLayerPoint& aVelocity) {
    mPreviousFlingCancelVelocity = aVelocity;
  }

 protected:
  bool ShouldAccelerate(const SampleTime& aNow,
                        const ParentLayerPoint& aVelocity,
                        const FlingHandoffState& aHandoffState) const;

  ParentLayerPoint mPreviousFlingStartingVelocity;
  ParentLayerPoint mPreviousFlingCancelVelocity;
  bool mIsTracking = false;
};

}  
}  

#endif  // mozilla_layers_FlingAccelerator_h
