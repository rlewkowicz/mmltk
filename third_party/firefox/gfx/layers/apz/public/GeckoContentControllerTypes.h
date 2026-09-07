/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_GeckoContentControllerTypes_h
#define mozilla_layers_GeckoContentControllerTypes_h

#include "mozilla/DefineEnum.h"

namespace mozilla {
namespace layers {

// clang-format off
MOZ_DEFINE_ENUM_CLASS(GeckoContentController_APZStateChange, (
  eTransformBegin,
  eTransformEnd,
  eStartTouch,
  eStartPanning,
  eEndTouch
));
// clang-format on

// clang-format off
MOZ_DEFINE_ENUM_CLASS(GeckoContentController_TapType, (
  eSingleTap,
  eDoubleTap,
  eSecondTap,
  eLongTap,
  eLongTapUp
));
// clang-format on

}  
}  

#endif  // mozilla_layers_GeckoContentControllerTypes_h
