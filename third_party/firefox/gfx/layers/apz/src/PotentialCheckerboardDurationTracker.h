/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_PotentialCheckerboardDurationTracker_h
#define mozilla_layers_PotentialCheckerboardDurationTracker_h

#include "mozilla/TimeStamp.h"

namespace mozilla {
namespace layers {

class PotentialCheckerboardDurationTracker {
 public:
  PotentialCheckerboardDurationTracker();

  void CheckerboardSeen();
  void CheckerboardDone(bool aRecordTelemetry);

  void InTransform(bool aInTransform, bool aRecordTelemetry);

 private:
  bool Tracking() const;

 private:
  bool mInCheckerboard;
  bool mInTransform;

  TimeStamp mCurrentPeriodStart;
};

}  
}  

#endif  // mozilla_layers_PotentialCheckerboardDurationTracker_h
