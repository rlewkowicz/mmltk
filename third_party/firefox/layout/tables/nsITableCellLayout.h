/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsITableCellLayout_h_
#define nsITableCellLayout_h_

#include "nsQueryFrame.h"

#define MAX_ROWSPAN 65534  // the cellmap can not handle more.
#define MAX_COLSPAN \
  1000  // limit as IE and opera do.  If this ever changes,

class nsITableCellLayout {
 public:
  NS_DECL_QUERYFRAME_TARGET(nsITableCellLayout)

  NS_IMETHOD GetCellIndexes(int32_t& aRowIndex, int32_t& aColIndex) = 0;
};

#endif
