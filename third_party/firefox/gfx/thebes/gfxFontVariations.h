/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_FONT_VARIATIONS_H
#define GFX_FONT_VARIATIONS_H

#include "mozilla/gfx/FontVariation.h"
#include "nsString.h"
#include "nsTArray.h"

typedef mozilla::gfx::FontVariation gfxFontVariation;

struct gfxFontVariationAxis {
  uint32_t mTag;
  nsCString mName;  
  float mMinValue;
  float mMaxValue;
  float mDefaultValue;
};

struct gfxFontVariationValue {
  uint32_t mAxis;
  float mValue;
};

struct gfxFontVariationInstance {
  nsCString mName;
  CopyableTArray<gfxFontVariationValue> mValues;
};

#endif
