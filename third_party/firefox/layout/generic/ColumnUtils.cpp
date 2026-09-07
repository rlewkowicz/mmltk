/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "mozilla/ColumnUtils.h"

#include <algorithm>

#include "nsContainerFrame.h"
#include "nsLayoutUtils.h"

namespace mozilla {

nscoord ColumnUtils::GetColumnGap(const nsContainerFrame* aFrame,
                                  nscoord aPercentageBasis) {
  const auto& columnGap = aFrame->StylePosition()->mColumnGap;
  if (columnGap.IsNormal()) {
    return aFrame->StyleFont()->mFont.size.ToAppUnits();
  }
  return nsLayoutUtils::ResolveGapToLength(columnGap, aPercentageBasis);
}

nscoord ColumnUtils::ClampUsedColumnWidth(const Length& aColumnWidth) {
  return std::max(AppUnitsPerCSSPixel(), aColumnWidth.ToAppUnits());
}

nscoord ColumnUtils::IntrinsicISize(uint32_t aColCount, nscoord aColGap,
                                    nscoord aColISize) {
  MOZ_ASSERT(aColCount > 0, "Cannot compute with zero columns!");

  nscoord iSize = aColISize * aColCount + aColGap * (aColCount - 1);

  return std::max(iSize, aColISize);
}

}  
