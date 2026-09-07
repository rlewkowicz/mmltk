/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(nsMathUtils_h_)
#define nsMathUtils_h_

#include "nscore.h"
#include <cmath>
#include <float.h>


inline double NS_round(double aNum) {
  return aNum >= 0.0 ? floor(aNum + 0.5) : ceil(aNum - 0.5);
}
inline float NS_roundf(float aNum) {
  return aNum >= 0.0f ? floorf(aNum + 0.5f) : ceilf(aNum - 0.5f);
}
inline int32_t NS_lround(double aNum) {
  return aNum >= 0.0 ? int32_t(aNum + 0.5) : int32_t(aNum - 0.5);
}

inline int32_t NS_lroundf(float aNum) {
  return aNum >= 0.0f ? int32_t(aNum + 0.5f) : int32_t(aNum - 0.5f);
}

inline double NS_hypot(double aNum1, double aNum2) {
#if defined(__GNUC__)
  return __builtin_hypot(aNum1, aNum2);
#else
  return hypot(aNum1, aNum2);
#endif
}

inline double NS_floorModulo(double aNum1, double aNum2) {
  return (aNum1 - aNum2 * floor(aNum1 / aNum2));
}

#endif
