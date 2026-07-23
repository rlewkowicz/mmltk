/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkMatrix22_DEFINED)
#define SkMatrix22_DEFINED

#include "include/core/SkPoint.h"

class SkMatrix;

void SkComputeGivensRotation(const SkVector& h, SkMatrix* G);

#endif
