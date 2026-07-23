/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_ScrollThumbUtils_h
#define mozilla_layers_ScrollThumbUtils_h

#include "LayersTypes.h"
#include "Units.h"

namespace mozilla {
namespace layers {

class AsyncPanZoomController;

struct FrameMetrics;
struct ScrollbarData;

namespace apz {
LayerToParentLayerMatrix4x4 ComputeTransformForScrollThumb(
    const LayerToParentLayerMatrix4x4& aCurrentTransform,
    const gfx::Matrix4x4& aScrollableContentTransform,
    AsyncPanZoomController* aApzc, const FrameMetrics& aMetrics,
    const ScrollbarData& aScrollbarData, bool aScrollbarIsDescendant);

}  
}  
}  

#endif  // mozilla_layers_ScrollThumbUtils_h
