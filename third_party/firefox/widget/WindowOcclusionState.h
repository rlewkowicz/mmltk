/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef widget_WindowOcclusionState_h
#define widget_WindowOcclusionState_h

#include <cstdint>

namespace mozilla {
namespace widget {

enum class OcclusionState : uint8_t {
  UNKNOWN = 0,
  VISIBLE = 1,
  OCCLUDED = 2,
  HIDDEN = 3,

  kMaxValue = HIDDEN,
};

}  
}  

#endif  // widget_WindowOcclusionState_h
