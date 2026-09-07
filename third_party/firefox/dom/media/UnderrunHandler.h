/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef MOZILLA_UNDERRUNHANDLER_H_
#define MOZILLA_UNDERRUNHANDLER_H_

namespace mozilla {
void InstallSoftRealTimeLimitHandler();
bool SoftRealTimeLimitReached();
void DemoteThreadFromRealTime();
}  

#endif  // MOZILLA_UNDERRUNHANDLER_H_
