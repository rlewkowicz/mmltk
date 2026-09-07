/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_CSSAlignUtils_h
#define mozilla_CSSAlignUtils_h

#include "mozilla/EnumSet.h"
#include "mozilla/WritingModes.h"

namespace mozilla {

struct ReflowInput;
struct StyleAlignFlags;

class CSSAlignUtils {
 public:
  static StyleAlignFlags UsedAlignmentForAbsPos(nsIFrame* aFrame,
                                                StyleAlignFlags aFlags,
                                                LogicalAxis aLogicalAxis,
                                                WritingMode aCBWM);

  enum class AlignJustifyFlag {
    OverflowSafe,

    SameSide,

    IgnoreAutoMargins,

    AligningMarginBox,

    LastBaselineSharingGroup,
  };
  using AlignJustifyFlags = EnumSet<AlignJustifyFlag>;

  struct AnchorAlignInfo {
    nscoord mAnchorStart;
    nscoord mAnchorSize;
  };

  static nscoord AlignJustifySelf(
      const StyleAlignFlags& aAlignment, LogicalAxis aAxis,
      AlignJustifyFlags aFlags, nscoord aBaselineAdjust, nscoord aCBSize,
      const ReflowInput& aRI, const LogicalSize& aChildSize,
      const Maybe<AnchorAlignInfo>& aAnchorRect = Nothing());
};

}  

#endif  // mozilla_CSSAlignUtils_h
