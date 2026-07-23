/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_PositionedEventTargeting_h
#define mozilla_PositionedEventTargeting_h

#include <stdint.h>

#include "RelativeTo.h"
#include "mozilla/EventForwards.h"

class nsIFrame;
struct nsPoint;

namespace mozilla {

enum { INPUT_IGNORE_ROOT_SCROLL_FRAME = 0x01 };
nsIFrame* FindFrameTargetedByInputEvent(
    WidgetGUIEvent* aEvent, RelativeTo aRootFrame,
    const nsPoint& aPointRelativeToRootFrame, uint32_t aFlags = 0);

class MOZ_RAII EventRetargetSuppression {
 public:
  EventRetargetSuppression();
  ~EventRetargetSuppression();
  static bool IsActive();

 private:
  static uint32_t sSuppressionCount;
};

}  

#endif /* mozilla_PositionedEventTargeting_h */
