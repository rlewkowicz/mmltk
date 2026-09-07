/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_
#define DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_

#include "FirstInitializationAttempts.h"
#include "mozilla/Assertions.h"
#include "nsError.h"

namespace mozilla::dom::quota {

template <typename Initialization, typename StringGenerator>
void FirstInitializationAttempts<Initialization, StringGenerator>::
    RecordFirstInitializationAttempt(const Initialization aInitialization,
                                     const nsresult) {
  MOZ_ASSERT(!FirstInitializationAttemptRecorded(aInitialization));

  mFirstInitializationAttempts |= aInitialization;
}

}  

#endif  // DOM_QUOTA_FIRSTINITIALIZATIONATTEMPTSIMPL_H_
