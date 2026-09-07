/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_CSSPropFlags_h
#define mozilla_CSSPropFlags_h

#include "mozilla/TypedEnumBits.h"

namespace mozilla {

enum class CSSPropFlags : uint16_t {
  Inaccessible = 1 << 0,

  EnabledInUASheets = 1 << 1,
  EnabledInChrome = 1 << 2,
  EnabledInUASheetsAndChrome = EnabledInUASheets | EnabledInChrome,
  EnabledMask = EnabledInUASheetsAndChrome,

  CanAnimateOnCompositor = 1 << 3,

  Internal = 1 << 4,

  SerializedByServo = 1 << 5,

  IsLogical = 1 << 6,

  AffectsLayout = 1 << 7,
  AffectsOverflow = 1 << 8,
  AffectsPaint = 1 << 9,
};

MOZ_MAKE_ENUM_CLASS_BITWISE_OPERATORS(CSSPropFlags)

}  

#endif  // mozilla_CSSPropFlags_h
