/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _mozilla_layers_AutoDirWheelDeltaAdjuster_h_
#define _mozilla_layers_AutoDirWheelDeltaAdjuster_h_

#include "Axis.h"                         // for AxisX, AxisY, Side
#include "mozilla/WheelHandlingHelper.h"  // for AutoDirWheelDeltaAdjuster

namespace mozilla {
namespace layers {

class MOZ_STACK_CLASS APZAutoDirWheelDeltaAdjuster final
    : public AutoDirWheelDeltaAdjuster {
 public:
  APZAutoDirWheelDeltaAdjuster(double& aDeltaX, double& aDeltaY,
                               const AxisX& aAxisX, const AxisY& aAxisY,
                               bool aIsHorizontalContentRightToLeft)
      : AutoDirWheelDeltaAdjuster(aDeltaX, aDeltaY),
        mAxisX(aAxisX),
        mAxisY(aAxisY),
        mIsHorizontalContentRightToLeft(aIsHorizontalContentRightToLeft) {}

 private:
  virtual bool CanScrollAlongXAxis() const override {
    return mAxisX.CanScroll();
  }
  virtual bool CanScrollAlongYAxis() const override {
    return mAxisY.CanScroll();
  }
  virtual bool CanScrollUpwards() const override {
    return mAxisY.CanScrollTo(eSideTop);
  }
  virtual bool CanScrollDownwards() const override {
    return mAxisY.CanScrollTo(eSideBottom);
  }
  virtual bool CanScrollLeftwards() const override {
    return mAxisX.CanScrollTo(eSideLeft);
  }
  virtual bool CanScrollRightwards() const override {
    return mAxisX.CanScrollTo(eSideRight);
  }
  virtual bool IsHorizontalContentRightToLeft() const override {
    return mIsHorizontalContentRightToLeft;
  }

  const AxisX& mAxisX;
  const AxisY& mAxisY;
  bool mIsHorizontalContentRightToLeft;
};

}  
}  

#endif  // _mozilla_layers_AutoDirWheelDeltaAdjuster_h_
