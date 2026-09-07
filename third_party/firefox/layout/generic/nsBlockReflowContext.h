/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsBlockReflowContext_h_
#define nsBlockReflowContext_h_

#include "mozilla/ReflowOutput.h"

class nsIFrame;
class nsLineBox;
class nsPresContext;
class nsReflowStatus;
namespace mozilla {
class BlockReflowState;
}  

class nsBlockReflowContext {
  using BlockReflowState = mozilla::BlockReflowState;
  using ReflowInput = mozilla::ReflowInput;
  using ReflowOutput = mozilla::ReflowOutput;

 public:
  nsBlockReflowContext(nsPresContext* aPresContext,
                       const ReflowInput& aParentRI);
  ~nsBlockReflowContext() = default;

  void ReflowBlock(const mozilla::LogicalRect& aSpace, bool aApplyBStartMargin,
                   mozilla::CollapsingMargin& aPrevMargin, nscoord aClearance,
                   nsLineBox* aLine, ReflowInput& aReflowInput,
                   nsReflowStatus& aReflowStatus, BlockReflowState& aState);

  bool PlaceBlock(const ReflowInput& aReflowInput, bool aForceFit,
                  nsLineBox* aLine,
                  mozilla::CollapsingMargin& aBEndMarginResult ,
                  mozilla::OverflowAreas& aOverflowAreas,
                  const nsReflowStatus& aReflowStatus);

  mozilla::CollapsingMargin& GetCarriedOutBEndMargin() {
    return mMetrics.mCarriedOutBEndMargin;
  }

  const ReflowOutput& GetMetrics() const { return mMetrics; }

  bool ComputeCollapsedBStartMargin(const ReflowInput& aRI,
                                    mozilla::CollapsingMargin* aMargin,
                                    nsIFrame* aClearanceFrame,
                                    bool* aMayNeedRetry,
                                    bool* aIsEmpty = nullptr);

 protected:
  nsPresContext* mPresContext;
  const ReflowInput& mOuterReflowInput;

  nsIFrame* mFrame;
  mozilla::LogicalRect mSpace;

  nscoord mICoord, mBCoord;
  nsSize mContainerSize;
  mozilla::WritingMode mWritingMode;
  ReflowOutput mMetrics;
  mozilla::CollapsingMargin mBStartMargin;
};

#endif /* nsBlockReflowContext_h_ */
