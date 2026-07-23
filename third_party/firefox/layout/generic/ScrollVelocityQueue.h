/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef ScrollVelocityQueue_h_
#define ScrollVelocityQueue_h_

#include "mozilla/TimeStamp.h"
#include "nsPoint.h"
#include "nsTArray.h"

class nsPresContext;

namespace mozilla {
namespace layout {


class ScrollVelocityQueue final {
 public:
  explicit ScrollVelocityQueue(nsPresContext* aPresContext)
      : mPresContext(aPresContext) {}

  void Sample(const nsPoint& aScrollPosition);

  void Reset();

  nsPoint GetVelocity();

 private:
  nsTArray<std::pair<uint32_t, nsPoint> > mQueue;

  nsPoint mAccumulator;

  TimeStamp mSampleTime;

  nsPoint mLastPosition;

  nsPresContext* mPresContext;

  void TrimQueue();
};

}  
}  

#endif /* !defined(ScrollVelocityQueue_h_) */
