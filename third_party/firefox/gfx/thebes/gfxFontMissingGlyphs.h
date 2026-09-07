/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_FONTMISSINGGLYPHS_H
#define GFX_FONTMISSINGGLYPHS_H

#include "mozilla/gfx/MatrixFwd.h"
#include "mozilla/gfx/Rect.h"

namespace mozilla {
namespace gfx {
class DrawTarget;
class Pattern;
}  
}  

class gfxFontMissingGlyphs final {
  typedef mozilla::gfx::DrawTarget DrawTarget;
  typedef mozilla::gfx::Float Float;
  typedef mozilla::gfx::Matrix Matrix;
  typedef mozilla::gfx::Pattern Pattern;
  typedef mozilla::gfx::Rect Rect;

 public:
  gfxFontMissingGlyphs() = delete;  

  static void DrawMissingGlyph(uint32_t aChar, const Rect& aRect,
                               DrawTarget& aDrawTarget, const Pattern& aPattern,
                               const Matrix* aMat = nullptr);
  static Float GetDesiredMinWidth(uint32_t aChar, uint32_t aAppUnitsPerDevUnit);

  static void Purge();

  static void Shutdown();
};

#endif
