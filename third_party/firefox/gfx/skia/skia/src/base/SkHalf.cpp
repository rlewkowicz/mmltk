/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/base/SkHalf.h"

#include "include/private/base/SkFloatingPoint.h"
#include "src/base/SkVx.h"

SkHalf SkFloatToHalf(float f) {
    if (std::isnan(f)) {
        return SK_HalfNaN;
    } else {
        return to_half(skvx::Vec<1,float>(f))[0];
    }
}

float SkHalfToFloat(SkHalf h) {
    return from_half(skvx::Vec<1,uint16_t>(h))[0];
}
