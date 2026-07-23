/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_TouchCounter_h
#define mozilla_layers_TouchCounter_h

#include "mozilla/EventForwards.h"

namespace mozilla {

class MultiTouchInput;

namespace layers {

class TouchCounter {
 public:
  TouchCounter();
  void Update(const MultiTouchInput& aInput);
  void Update(const WidgetTouchEvent& aEvent);
  uint32_t GetActiveTouchCount() const;
  bool HasSeenFirstMove() const { return mFirstMoveSeen; }

 private:
  uint32_t mActiveTouchCount;
  bool mFirstMoveSeen;
};

}  
}  

#endif /* mozilla_layers_TouchCounter_h */
