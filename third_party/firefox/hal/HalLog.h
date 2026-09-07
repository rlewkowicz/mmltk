/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_HalLog_h
#define mozilla_HalLog_h

#include "mozilla/Logging.h"


namespace mozilla {

namespace hal {

mozilla::LogModule* GetHalLog();
#define HAL_LOG(...) \
  MOZ_LOG(mozilla::hal::GetHalLog(), LogLevel::Debug, (__VA_ARGS__))
#define HAL_ERR(...) \
  MOZ_LOG(mozilla::hal::GetHalLog(), LogLevel::Error, (__VA_ARGS__))

}  

}  

#endif  // mozilla_HalLog_h
