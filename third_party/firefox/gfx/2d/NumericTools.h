/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_GFX_NUMERICTOOLS_H_
#define MOZILLA_GFX_NUMERICTOOLS_H_

#include <cstdint>

namespace mozilla {


inline int32_t RoundDownToMultiple(int32_t x, int32_t aMultiplier) {
  int mod = x % aMultiplier;
  if (x > 0) {
    return x - mod;
  }
  return mod ? x - aMultiplier - mod : x;
}

inline int32_t RoundUpToMultiple(int32_t x, int32_t aMultiplier) {
  int mod = x % aMultiplier;
  if (x > 0) {
    return mod ? x + aMultiplier - mod : x;
  }
  return x - mod;
}

inline int32_t RoundToMultiple(int32_t x, int32_t aMultiplier) {
  return RoundDownToMultiple(x + aMultiplier / 2, aMultiplier);
}

}  

#endif /* MOZILLA_GFX_NUMERICTOOLS_H_ */
