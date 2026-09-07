/*
 * Copyright 2014 Google Inc.
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkDashPathPriv_DEFINED)
#define SkDashPathPriv_DEFINED

#include "include/core/SkPathEffect.h"
#include "include/core/SkSpan.h"
#include "src/core/SkPathEffectBase.h"

namespace SkDashPath {
void CalcDashParameters(SkScalar phase, SkSpan<const SkScalar> intervals,
                        SkScalar* initialDashLength, size_t* initialDashIndex,
                        SkScalar* intervalLength, SkScalar* adjustedPhase = nullptr);

bool FilterDashPath(SkPathBuilder* dst, const SkPath& src, SkStrokeRec*, const SkRect*,
                    const SkPathEffectBase::DashInfo& info);

const SkScalar kMaxDashCount = 1000000;

    enum class StrokeRecApplication {
        kDisallow,
        kAllow,
    };

    bool InternalFilter(SkPathBuilder* dst, const SkPath& src, SkStrokeRec* rec,
                        const SkRect* cullRect, SkSpan<const SkScalar> aIntervals,
                        SkScalar initialDashLength, int32_t initialDashIndex,
                        SkScalar intervalLength, SkScalar startPhase,
                        StrokeRecApplication = StrokeRecApplication::kAllow);

    bool ValidDashPath(SkScalar phase, SkSpan<const SkScalar> intervals);

}  

#endif
