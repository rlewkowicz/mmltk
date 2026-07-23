/*
 * Copyright 2006 The Android Open Source Project
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkMaskFilter_DEFINED)
#define SkMaskFilter_DEFINED

#include "include/core/SkFlattenable.h"
#include "include/core/SkRefCnt.h"
#include "include/core/SkScalar.h"
#include "include/core/SkTypes.h"

#include <cstddef>

enum SkBlurStyle : int;
struct SkDeserialProcs;

class SK_API SkMaskFilter : public SkFlattenable {
public:
    static sk_sp<SkMaskFilter> MakeBlur(SkBlurStyle style, SkScalar sigma,
                                        bool respectCTM = true);

    static sk_sp<SkMaskFilter> Deserialize(const void* data, size_t size,
                                           const SkDeserialProcs* procs = nullptr);

private:
    static void RegisterFlattenables();
    friend class SkFlattenable;
};

#endif
