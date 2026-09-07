/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkDashPathEffect_DEFINED)
#define SkDashPathEffect_DEFINED

#include "include/core/SkPathEffect.h"  // IWYU pragma: keep    (for unspanned apis)
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"

class SK_API SkDashPathEffect {
public:
    static sk_sp<SkPathEffect> Make(SkSpan<const SkScalar> intervals, SkScalar phase);
};

#endif
