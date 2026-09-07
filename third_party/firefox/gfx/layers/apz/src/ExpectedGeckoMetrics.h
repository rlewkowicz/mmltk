/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_ExpectedGeckoMetrics_h
#define mozilla_layers_ExpectedGeckoMetrics_h

#include "Units.h"

namespace mozilla {
namespace layers {

struct FrameMetrics;

class ExpectedGeckoMetrics {
 public:
  ExpectedGeckoMetrics() = default;
  void UpdateFrom(const FrameMetrics& aMetrics);
  void UpdateZoomFrom(const FrameMetrics& aMetrics);

  const CSSPoint& GetVisualScrollOffset() const { return mVisualScrollOffset; }
  const CSSPoint& GetLayoutScrollOffset() const { return mLayoutScrollOffset; }
  const CSSToParentLayerScale& GetZoom() const { return mZoom; }
  const CSSToLayoutDeviceScale& GetDevPixelsPerCSSPixel() const {
    return mDevPixelsPerCSSPixel;
  }

 private:
  CSSPoint mVisualScrollOffset;
  CSSPoint mLayoutScrollOffset;
  CSSToParentLayerScale mZoom;
  CSSToLayoutDeviceScale mDevPixelsPerCSSPixel;
};

}  
}  

#endif
