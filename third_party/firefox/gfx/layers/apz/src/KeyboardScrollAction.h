/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_layers_KeyboardScrollAction_h
#define mozilla_layers_KeyboardScrollAction_h

#include <cstdint>  // for uint8_t

#include "mozilla/ScrollTypes.h"
#include "mozilla/DefineEnum.h"  // for MOZ_DEFINE_ENUM

namespace mozilla {
namespace layers {

struct KeyboardScrollAction final {
 public:
  // clang-format off
  MOZ_DEFINE_ENUM_WITH_BASE_AT_CLASS_SCOPE(
    KeyboardScrollActionType, uint8_t, (
      eScrollCharacter,
      eScrollLine,
      eScrollPage,
      eScrollComplete
  ));
  // clang-format on

  static ScrollUnit GetScrollUnit(KeyboardScrollActionType aDeltaType);

  KeyboardScrollAction();
  KeyboardScrollAction(KeyboardScrollActionType aType, bool aForward);

  KeyboardScrollActionType mType;
  bool mForward;
};

}  
}  

#endif  // mozilla_layers_KeyboardScrollAction_h
