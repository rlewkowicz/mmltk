/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/ServiceWorkerLifetimeExtension.h"

namespace mozilla::dom {

bool ServiceWorkerLifetimeExtension::LifetimeExtendsIntoTheFuture(
    uint32_t aRequiredSecs) const {
  return this->match(
      [](const NoLifetimeExtension& nle) { return false; },
      [aRequiredSecs](const PropagatedLifetimeExtension& ple) {
        TimeStamp minFuture =
            TimeStamp::NowLoRes() + TimeDuration::FromSeconds(aRequiredSecs);

        return !(ple.mDeadline.IsNull() || ple.mDeadline < minFuture);
      },
      [](const FullLifetimeExtension& fle) { return true; });
}

}  
