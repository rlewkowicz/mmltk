/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef nsRubyContentFrame_h_
#define nsRubyContentFrame_h_

#include "nsInlineFrame.h"

class nsRubyContentFrame : public nsInlineFrame {
 public:
  NS_DECL_ABSTRACT_FRAME(nsRubyContentFrame)

  bool IsIntraLevelWhitespace() const;

 protected:
  nsRubyContentFrame(ComputedStyle* aStyle, nsPresContext* aPresContext,
                     ClassID aID)
      : nsInlineFrame(aStyle, aPresContext, aID) {}
};

#endif /* nsRubyContentFrame_h_ */
