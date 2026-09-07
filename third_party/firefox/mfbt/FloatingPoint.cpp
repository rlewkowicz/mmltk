/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#include "mozilla/FloatingPoint.h"

#include <cfloat>  // for FLT_MAX
#include <cmath>

namespace mozilla {

bool IsFloat32Representable(double aValue) {
  if (!std::isfinite(aValue)) {
    return true;
  }

  if (Abs(aValue) > FLT_MAX) {
    return false;
  }

  auto valueAsFloat = static_cast<float>(aValue);

  auto valueAsFloatAsDouble = static_cast<double>(valueAsFloat);

  return valueAsFloatAsDouble == aValue;
}

} 
