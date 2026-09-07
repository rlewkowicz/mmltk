/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsMathMLmrowFrame_h_
#define nsMathMLmrowFrame_h_

#include "nsMathMLContainerFrame.h"

namespace mozilla {
class PresShell;
}  


class nsMathMLmrowFrame final : public nsMathMLContainerFrame {
 public:
  NS_DECL_FRAMEARENA_HELPERS(nsMathMLmrowFrame)

  friend nsIFrame* NS_NewMathMLmrowFrame(mozilla::PresShell* aPresShell,
                                         ComputedStyle* aStyle);

  nsresult AttributeChanged(int32_t aNameSpaceID, nsAtom* aAttribute,
                            AttrModType aModType) override;

  NS_IMETHOD
  InheritAutomaticData(nsIFrame* aParent) override;

  NS_IMETHOD
  TransmitAutomaticData() override {
    return TransmitAutomaticDataForMrowLikeElement();
  }

  MathMLFrameType GetMathMLFrameType() override;

  bool IsMrowLike() override {
    return mFrames.FirstChild() != mFrames.LastChild() || !mFrames.FirstChild();
  }

 protected:
  explicit nsMathMLmrowFrame(ComputedStyle* aStyle, nsPresContext* aPresContext)
      : nsMathMLContainerFrame(aStyle, aPresContext, kClassID) {}
  virtual ~nsMathMLmrowFrame();
};

#endif /* nsMathMLmrowFrame_h_ */
