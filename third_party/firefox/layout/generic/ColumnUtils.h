/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_ColumnUtils_h
#define mozilla_ColumnUtils_h

#include "nsCoord.h"
#include "nsStyleConsts.h"

class nsContainerFrame;

namespace mozilla {

class ColumnUtils final {
 public:
  static nscoord GetColumnGap(const nsContainerFrame* aFrame,
                              nscoord aPercentageBasis);

  static nscoord ClampUsedColumnWidth(const Length& aColumnWidth);

  static nscoord IntrinsicISize(uint32_t aColCount, nscoord aColGap,
                                nscoord aColISize);
};

}  

#endif  // mozilla_ColumnUtils_h
