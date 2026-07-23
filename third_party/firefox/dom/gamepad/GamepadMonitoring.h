/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_GamepadMonitoring_h_
#define mozilla_dom_GamepadMonitoring_h_
#include "mozilla/dom/GamepadHandle.h"

namespace mozilla::dom {

void StartGamepadMonitoring();
void StopGamepadMonitoring();
void SetGamepadLightIndicatorColor(const Tainted<GamepadHandle>& aGamepadHandle,
                                   const Tainted<uint32_t>& aLightColorIndex,
                                   const uint8_t& aRed, const uint8_t& aGreen,
                                   const uint8_t& aBlue);

}  

#endif
