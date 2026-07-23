/*
 * Copyright 2017 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkFloatToDecimal_DEFINED)
#define SkFloatToDecimal_DEFINED

constexpr unsigned kMaximumSkFloatToDecimalLength = 49;

unsigned SkFloatToDecimal(float value, char output[kMaximumSkFloatToDecimalLength]);

#endif
