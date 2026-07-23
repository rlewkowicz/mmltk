/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/layers/APZUtils.h"

#include "mozilla/StaticPrefs_apz.h"
#include "mozilla/StaticPrefs_layers.h"

namespace mozilla {
namespace layers {

namespace apz {

bool IsStuckAtBottom(gfxFloat aTranslation,
                     const LayerRectAbsolute& aInnerRange,
                     const LayerRectAbsolute& aOuterRange) {
  return aOuterRange.Y() <= -aTranslation && -aTranslation <= aInnerRange.Y();
}

bool IsStuckAtTop(gfxFloat aTranslation, const LayerRectAbsolute& aInnerRange,
                  const LayerRectAbsolute& aOuterRange) {
  return aInnerRange.YMost() <= -aTranslation &&
         -aTranslation <= aOuterRange.YMost();
}

bool AboutToCheckerboard(const FrameMetrics& aPaintedMetrics,
                         const FrameMetrics& aCompositorMetrics) {
  CSSRect painted = aPaintedMetrics.GetDisplayPort() +
                    aPaintedMetrics.GetLayoutScrollOffset();
  painted.Inflate(CSSMargin::FromAppUnits(nsMargin(1, 1, 1, 1)));

  CSSRect visible =
      CSSRect(aCompositorMetrics.GetVisualScrollOffset(),
              aCompositorMetrics.CalculateBoundedCompositedSizeInCssPixels());
  visible.Inflate(ScreenSize(StaticPrefs::apz_danger_zone_x(),
                             StaticPrefs::apz_danger_zone_y()) /
                  aCompositorMetrics.DisplayportPixelsPerCSSPixel());

  painted = painted.Intersect(aPaintedMetrics.GetScrollableRect());
  visible = visible.Intersect(aPaintedMetrics.GetScrollableRect());

  return !painted.Contains(visible);
}

SideBits GetOverscrollSideBits(const ParentLayerPoint& aOverscrollAmount) {
  SideBits sides = SideBits::eNone;

  if (aOverscrollAmount.x < 0) {
    sides |= SideBits::eLeft;
  } else if (aOverscrollAmount.x > 0) {
    sides |= SideBits::eRight;
  }

  if (aOverscrollAmount.y < 0) {
    sides |= SideBits::eTop;
  } else if (aOverscrollAmount.y > 0) {
    sides |= SideBits::eBottom;
  }

  return sides;
}

}  
}  
}  
