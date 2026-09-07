/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMPPlatform_h_
#define GMPPlatform_h_

#include "gmp-platform.h"
namespace mozilla {
namespace gmp {

class GMPChild;

void InitPlatformAPI(GMPPlatformAPI& aPlatformAPI, GMPChild* aChild);

GMPErr RunOnMainThread(GMPTask* aTask);

GMPTask* NewGMPTask(std::function<void()>&& aFunction);

GMPErr SetTimerOnMainThread(GMPTask* aTask, int64_t aTimeoutMS);

}  
}  

#endif  // GMPPlatform_h_
