/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DrawMode_h
#define DrawMode_h

#include "mozilla/TypedEnumBits.h"

enum class DrawMode : int {
  GLYPH_FILL = 1 << 0,
  GLYPH_STROKE = 1 << 1,
  GLYPH_PATH = 1 << 2,
  GLYPH_STROKE_UNDERNEATH = 1 << 3
};
MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(DrawMode)
#endif
