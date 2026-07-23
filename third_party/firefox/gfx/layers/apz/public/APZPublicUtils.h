/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_APZPublicUtils_h
#define mozilla_layers_APZPublicUtils_h


#include <stdint.h>
#include "ScrollAnimationBezierPhysics.h"
#include "Units.h"
#include "mozilla/DefineEnum.h"
#include "mozilla/ScrollOrigin.h"
#include "mozilla/gfx/Point.h"
#include "mozilla/ScrollTypes.h"

namespace mozilla {

namespace layers {

struct FrameMetrics;

// clang-format off
MOZ_DEFINE_ENUM_CLASS_WITH_BASE(APZWheelAction, uint8_t, (
    Scroll,
    PinchZoom
))
// clang-format on

enum class DispatchToContent : bool { No, Yes };

namespace apz {

void InitializeGlobalState();

const ScreenMargin CalculatePendingDisplayPort(
    const FrameMetrics& aFrameMetrics, const ParentLayerPoint& aVelocity);

gfx::Size GetDisplayportAlignmentMultiplier(const ScreenSize& aBaseSize);

ScrollAnimationBezierPhysicsSettings ComputeBezierAnimationSettingsForOrigin(
    ScrollOrigin aOrigin);

ScrollMode GetScrollModeForOrigin(ScrollOrigin origin);

enum class ScrollAnimationKind : uint8_t {
  Smooth,
  SmoothMsd,
  Keyboard,
  Wheel
};

}  

}  
}  

#endif  // mozilla_layers_APZPublicUtils_h
