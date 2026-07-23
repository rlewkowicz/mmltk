/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_EffectsInfo_h
#define mozilla_dom_EffectsInfo_h

#include "Units.h"
#include "nsRect.h"

namespace mozilla::dom {

class EffectsInfo {
 public:
  EffectsInfo() = default;

  static EffectsInfo VisibleWithinRect(
      const Maybe<nsRect>& aVisibleRect, const Scale2D& aRasterScale,
      const ParentLayerToScreenScale2D& aTransformToAncestorScale) {
    return EffectsInfo{aVisibleRect, aRasterScale, aTransformToAncestorScale};
  }
  static EffectsInfo FullyHidden() { return {}; }

  bool operator==(const EffectsInfo& aOther) const {
    return mVisibleRect == aOther.mVisibleRect &&
           mRasterScale == aOther.mRasterScale &&
           mTransformToAncestorScale == aOther.mTransformToAncestorScale;
  }
  bool operator!=(const EffectsInfo& aOther) const {
    return !(*this == aOther);
  }

  bool IsVisible() const { return mVisibleRect.isSome(); }

  Maybe<nsRect> mVisibleRect;
  Scale2D mRasterScale;
  ParentLayerToScreenScale2D mTransformToAncestorScale;


 private:
  EffectsInfo(const Maybe<nsRect>& aVisibleRect, const Scale2D& aRasterScale,
              const ParentLayerToScreenScale2D& aTransformToAncestorScale)
      : mVisibleRect(aVisibleRect),
        mRasterScale(aRasterScale),
        mTransformToAncestorScale(aTransformToAncestorScale) {}
};

}  

#endif  // mozilla_dom_EffectsInfo_h
