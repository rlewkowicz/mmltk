/*
 * Copyright 2009 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */


#if !defined(SkCubicClipper_DEFINED)
#define SkCubicClipper_DEFINED

#include "include/core/SkRect.h"
#include "include/core/SkScalar.h"
#include "include/core/SkTypes.h"

struct SkPoint;

class SkCubicClipper {
public:
    SkCubicClipper();

    void setClip(const SkIRect& clip);

    [[nodiscard]] bool clipCubic(const SkPoint src[4], SkPoint dst[4]);

    [[nodiscard]] static bool ChopMonoAtY(const SkPoint pts[4], SkScalar y, SkScalar* t);
private:
    SkRect      fClip;
};

#endif
