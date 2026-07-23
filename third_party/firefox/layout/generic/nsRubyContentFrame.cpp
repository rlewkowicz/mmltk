/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "nsRubyContentFrame.h"

#include "mozilla/ComputedStyle.h"
#include "nsPresContext.h"

using namespace mozilla;



bool nsRubyContentFrame::IsIntraLevelWhitespace() const {
  auto pseudoType = Style()->GetPseudoType();
  if (pseudoType != PseudoStyleType::MozRubyBase &&
      pseudoType != PseudoStyleType::MozRubyText) {
    return false;
  }

  nsIFrame* child = mFrames.OnlyChild();
  return child && child->GetContent()->TextIsOnlyWhitespace();
}
