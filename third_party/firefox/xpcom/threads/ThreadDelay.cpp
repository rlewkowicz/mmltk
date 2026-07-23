/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ThreadDelay.h"
#include "mozilla/Assertions.h"
#include "mozilla/ChaosMode.h"

#  include <unistd.h>

namespace mozilla {

void DelayForChaosMode(ChaosFeature aFeature,
                       const uint32_t aMicrosecondLimit) {
  if (!ChaosMode::isActive(aFeature)) {
    return;
  }

  MOZ_ASSERT(aMicrosecondLimit <= 1000);
  const uint32_t duration = ChaosMode::randomUint32LessThan(aMicrosecondLimit);
  ::usleep(duration);
}

}  
