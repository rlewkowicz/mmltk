/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_AnonymousContentKey_h
#define mozilla_AnonymousContentKey_h

#include <stdint.h>

#include "mozilla/TypedEnumBits.h"

namespace mozilla {

// clang-format off

enum class AnonymousContentKey : uint8_t {
  None                           = 0x00,

  Type_ScrollCorner              = 0x01,
  Type_Scrollbar                 = 0x02,
  Type_ScrollbarButton           = 0x03,
  Type_Slider                    = 0x04,

  Flag_Vertical                  = 0x08,

  Flag_ScrollbarButton_Down      = 0x10,
  Flag_ScrollbarButton_Bottom    = 0x20,
  Flag_ScrollbarButton_Decrement = 0x40,
};

// clang-format on

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(AnonymousContentKey)

}  

#endif  // mozilla_AnonymousContentKey_h
