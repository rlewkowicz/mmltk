/*
 * Copyright 2019 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#if !defined(SkCharToGlyphCache_DEFINED)
#define SkCharToGlyphCache_DEFINED

#include "include/core/SkTypes.h"
#include "include/private/base/SkTDArray.h"
#include "include/private/base/SkTo.h"

class SkCharToGlyphCache {
public:
    SkCharToGlyphCache();
    ~SkCharToGlyphCache();

    int count() const {
        return fKUnichar.size();
    }

    void reset();       

    int findGlyphIndex(SkUnichar c) const;

    void insertCharAndGlyph(int index, SkUnichar, SkGlyphID);

    void addCharAndGlyph(SkUnichar unichar, SkGlyphID glyph) {
        int index = this->findGlyphIndex(unichar);
        if (index >= 0) {
            SkASSERT(SkToU16(index) == glyph);
        } else {
            this->insertCharAndGlyph(~index, unichar, glyph);
        }
    }

private:
    SkTDArray<SkUnichar> fKUnichar;
    SkTDArray<SkGlyphID> fVGlyph;
    double               fDenom;
};

#endif
