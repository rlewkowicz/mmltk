/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_FocusModel_h
#define mozilla_FocusModel_h

#include "mozilla/TypedEnumBits.h"

namespace mozilla {

enum class IsFocusableFlags : uint8_t {
  WithMouse = 1 << 0,
  IgnoreVisibility = 1 << 1,
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(IsFocusableFlags);

enum class TabFocusableType : uint8_t {
  TextControls = 1,       
  FormElements = 1 << 1,  
  Links = 1 << 2,         
  Any = TextControls | FormElements | Links,
};

class FocusModel final {
 public:
  static constexpr bool AppliesToXUL() { return false; }

  static constexpr bool IsTabFocusable(TabFocusableType) { return true; }
};

}  

#endif
