/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef _nsBoundingMetrics_h
#define _nsBoundingMetrics_h

#include "nsCoord.h"
#include <algorithm>


struct nsBoundingMetrics {




  nscoord leftBearing;

  nscoord rightBearing;

  nscoord ascent;

  nscoord descent;

  nscoord width;

  nsBoundingMetrics()
      : leftBearing(0), rightBearing(0), ascent(0), descent(0), width(0) {}

  void operator+=(const nsBoundingMetrics& bm) {
    if (ascent + descent == 0 && rightBearing - leftBearing == 0) {
      ascent = bm.ascent;
      descent = bm.descent;
      leftBearing = width + bm.leftBearing;
      rightBearing = width + bm.rightBearing;
    } else {
      if (ascent < bm.ascent) ascent = bm.ascent;
      if (descent < bm.descent) descent = bm.descent;
      leftBearing = std::min(leftBearing, width + bm.leftBearing);
      rightBearing = std::max(rightBearing, width + bm.rightBearing);
    }
    width += bm.width;
  }
};

#endif  // _nsBoundingMetrics_h
