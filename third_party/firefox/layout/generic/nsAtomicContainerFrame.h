/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsAtomicContainerFrame_h_
#define nsAtomicContainerFrame_h_

#include "nsContainerFrame.h"

class nsAtomicContainerFrame : public nsContainerFrame {
 public:
  NS_DECL_ABSTRACT_FRAME(nsAtomicContainerFrame)

  FrameSearchResult PeekOffsetNoAmount(bool aForward,
                                       int32_t* aOffset) override {
    return nsIFrame::PeekOffsetNoAmount(aForward, aOffset);
  }
  FrameSearchResult PeekOffsetCharacter(
      bool aForward, int32_t* aOffset,
      PeekOffsetCharacterOptions aOptions =
          PeekOffsetCharacterOptions()) override {
    return nsIFrame::PeekOffsetCharacter(aForward, aOffset, aOptions);
  }

 protected:
  nsAtomicContainerFrame(ComputedStyle* aStyle, nsPresContext* aPresContext,
                         ClassID aID)
      : nsContainerFrame(aStyle, aPresContext, aID) {}
};

#endif  // nsAtomicContainerFrame_h_
