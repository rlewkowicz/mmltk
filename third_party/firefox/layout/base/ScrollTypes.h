/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_ScrollTypes_h
#define mozilla_ScrollTypes_h

#include "mozilla/DefineEnum.h"
#include "mozilla/TypedEnumBits.h"


namespace mozilla {

MOZ_DEFINE_ENUM_CLASS_WITH_BASE_AND_TOSTRING(ScrollMode, uint8_t,
                                             (Instant, Smooth, SmoothMsd,
                                              Normal));

enum class ScrollUnit { DEVICE_PIXELS, LINES, PAGES, WHOLE };

enum class APZScrollAnimationType : uint8_t {
  No,                   
  TriggeredByScript,    
  TriggeredByUserInput  
};

enum class ScrollSnapFlags : uint8_t {
  Disabled = 0,
  IntendedEndPosition = 1 << 0,
  IntendedDirection = 1 << 1
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(ScrollSnapFlags);

}  

#endif  // mozilla_ScrollTypes_h
