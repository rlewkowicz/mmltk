/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_CSSEnabledState_h
#define mozilla_CSSEnabledState_h

#include "mozilla/TypedEnumBits.h"

namespace mozilla {

enum class CSSEnabledState {
  ForAllContent = 0,
  InUASheets = 0x01,
  InChrome = 0x02,
  IgnoreEnabledState = 0xff
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(CSSEnabledState)

}  

#endif  // mozilla_CSSEnabledState_h
