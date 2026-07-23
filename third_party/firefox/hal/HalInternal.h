/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_HalInternal_h
#define mozilla_HalInternal_h 1


#ifndef MOZ_HAL_NAMESPACE
#  error "You shouldn't directly include HalInternal.h!"
#endif

namespace mozilla {
namespace MOZ_HAL_NAMESPACE {

void EnableBatteryNotifications();

void DisableBatteryNotifications();

void EnableNetworkNotifications();

void DisableNetworkNotifications();

void EnableScreenConfigurationNotifications();

void DisableScreenConfigurationNotifications();

bool HalChildDestroyed();
}  
}  

#endif  // mozilla_HalInternal_h
