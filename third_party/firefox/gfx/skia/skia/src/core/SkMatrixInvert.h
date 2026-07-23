/*
 * Copyright 2021 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkMatrixInvert_DEFINED)
#define SkMatrixInvert_DEFINED

#include "include/core/SkScalar.h"

SkScalar SkInvert2x2Matrix(const SkScalar inMatrix[4], SkScalar outMatrix[4]);
SkScalar SkInvert3x3Matrix(const SkScalar inMatrix[9], SkScalar outMatrix[9]);
SkScalar SkInvert4x4Matrix(const SkScalar inMatrix[16], SkScalar outMatrix[16]);

#endif
