/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef LAYOUT_SVG_DISPLAYSVGITEM_H_
#define LAYOUT_SVG_DISPLAYSVGITEM_H_

#include "nsDisplayList.h"

namespace mozilla {


class DisplaySVGItem : public nsPaintedDisplayItem {
 public:
  DisplaySVGItem(nsDisplayListBuilder* aBuilder, nsIFrame* aFrame)
      : nsPaintedDisplayItem(aBuilder, aFrame) {
    MOZ_ASSERT(aFrame, "Must have a frame!");
  }

  void HitTest(nsDisplayListBuilder* aBuilder, const nsRect& aRect,
               HitTestState* aState, nsTArray<nsIFrame*>* aOutFrames) override;
  void Paint(nsDisplayListBuilder* aBuilder, gfxContext* aCtx) override;
};

}  

#endif  // LAYOUT_SVG_DISPLAYSVGITEM_H_
