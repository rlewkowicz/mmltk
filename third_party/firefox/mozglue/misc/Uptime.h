/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_Uptime_h
#define mozilla_Uptime_h

#include <stdint.h>

#include "mozilla/Maybe.h"

namespace mozilla {

MFBT_API void InitializeUptime();
MFBT_API Maybe<uint64_t> ProcessUptimeMs();
MFBT_API Maybe<uint64_t> ProcessUptimeExcludingSuspendMs();

};  

#endif  // mozilla_Uptime_h
