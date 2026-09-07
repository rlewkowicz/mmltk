/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GFX_HITTESTINFOMANAGER_H
#define GFX_HITTESTINFOMANAGER_H

#include "mozilla/gfx/CompositorHitTestInfo.h"
#include "mozilla/layers/ScrollableLayerGuid.h"
#include "mozilla/webrender/WebRenderAPI.h"
#include "nsRect.h"

namespace mozilla {

class nsDisplayItem;
class nsDisplayListBuilder;

namespace wr {
class DisplayListBuilder;
}

namespace layers {

class HitTestInfoManager {
 public:
  HitTestInfoManager();

  void Reset();

  bool ProcessItem(nsDisplayItem* aItem, wr::DisplayListBuilder& aBuilder,
                   nsDisplayListBuilder* aDisplayListBuilder);

  void ProcessItemAsImage(nsDisplayItem* aItem, const wr::LayoutRect& aRect,
                          wr::DisplayListBuilder& aBuilder,
                          nsDisplayListBuilder* aDisplayListBuilder);

 private:
  bool Update(const nsRect& aArea, const gfx::CompositorHitTestInfo& aFlags,
              const ScrollableLayerGuid::ViewID& aViewId,
              const wr::WrSpaceAndClipChain& aSpaceAndClip);

  nsRect mArea;
  gfx::CompositorHitTestInfo mFlags;
  ScrollableLayerGuid::ViewID mViewId;
  wr::WrSpaceAndClipChain mSpaceAndClipChain;
};

}  
}  

#endif
