/*
 * Copyright 2022 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkOpenTypeSVGDecoder_DEFINED)
#define SkOpenTypeSVGDecoder_DEFINED

#include "include/core/SkColor.h"
#include "include/core/SkSpan.h"
#include "include/core/SkTypes.h"

#include <memory>

class SkCanvas;

class SkOpenTypeSVGDecoder {
public:
    virtual size_t approximateSize() = 0;
    virtual bool render(SkCanvas&, int upem, SkGlyphID glyphId,
                        SkColor foregroundColor, SkSpan<SkColor> palette) = 0;
    virtual ~SkOpenTypeSVGDecoder() = default;
};

#endif
