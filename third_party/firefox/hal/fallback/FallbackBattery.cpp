/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "Hal.h"

namespace mozilla::hal_impl {

void EnableBatteryNotifications() {}

void DisableBatteryNotifications() {}

void GetCurrentBatteryInformation(hal::BatteryInformation* aBatteryInfo) {
  aBatteryInfo->level() = 1.0;
  aBatteryInfo->charging() = true;
  aBatteryInfo->remainingTime() = 0.0;
}

}  
