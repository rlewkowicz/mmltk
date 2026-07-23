/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DDTimeStamp_h_
#define DDTimeStamp_h_

#include "mozilla/TimeStamp.h"

namespace mozilla {

using DDTimeStamp = TimeStamp;

inline DDTimeStamp DDNow() { return TimeStamp::Now(); }

double ToSeconds(const DDTimeStamp& aTimeStamp);

}  

#endif  // DDTimeStamp_h_
