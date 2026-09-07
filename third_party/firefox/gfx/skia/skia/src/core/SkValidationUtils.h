/*
 * Copyright 2013 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkValidationUtils_DEFINED)
#define SkValidationUtils_DEFINED

#include "include/core/SkBitmap.h"
#include "include/core/SkBlendMode.h"

static inline bool SkIsValidMode(SkBlendMode mode) {
    return (unsigned)mode <= (unsigned)SkBlendMode::kLastMode;
}

static inline bool SkIsValidIRect(const SkIRect& rect) {
    return rect.width() >= 0 && rect.height() >= 0;
}

static inline bool SkIsValidRect(const SkRect& rect) {
    return (rect.fLeft <= rect.fRight) &&
           (rect.fTop <= rect.fBottom) &&
           SkIsFinite(rect.width(), rect.height());
}

#endif
