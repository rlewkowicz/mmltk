/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_WRHitTester_h
#define mozilla_layers_WRHitTester_h

#include "IAPZHitTester.h"

namespace mozilla {
namespace layers {

class WRHitTester : public IAPZHitTester {
 public:
  virtual HitTestResult GetAPZCAtPoint(
      const ScreenPoint& aHitTestPoint,
      const RecursiveMutexAutoLock& aProofOfTreeLock) override;
};

}  
}  

#endif  // define mozilla_layers_WRHitTester_h
