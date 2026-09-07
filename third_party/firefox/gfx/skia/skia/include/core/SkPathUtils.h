/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#if !defined(SkPathUtils_DEFINED)
#define SkPathUtils_DEFINED

#include "include/core/SkScalar.h"  // IWYU pragma: keep
#include "include/core/SkTypes.h"

class SkMatrix;
class SkPaint;
class SkPath;
class SkPathBuilder;
struct SkRect;

namespace skpathutils {

SK_API bool FillPathWithPaint(const SkPath& src, const SkPaint& paint, SkPathBuilder* dst,
                              const SkRect* cullRect, const SkMatrix& ctm);

SK_API bool FillPathWithPaint(const SkPath& src, const SkPaint& paint, SkPathBuilder* dst);

SK_API SkPath FillPathWithPaint(const SkPath& src, const SkPaint& paint, bool* isFill = nullptr);

}

#endif
