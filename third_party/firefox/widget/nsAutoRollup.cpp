/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/widget/nsAutoRollup.h"

namespace mozilla {
namespace widget {

uint32_t nsAutoRollup::sCount = 0;
StaticRefPtr<nsIContent> nsAutoRollup::sLastRollup;

nsAutoRollup::nsAutoRollup() {
  mWasClear = !sLastRollup;
  sCount++;
}

nsAutoRollup::nsAutoRollup(nsIContent* aRollup) {
  MOZ_ASSERT(!sLastRollup);
  mWasClear = true;
  sCount++;
  SetLastRollup(aRollup);
}

nsAutoRollup::~nsAutoRollup() {
  if (sLastRollup && mWasClear) {
    sLastRollup = nullptr;
  }
  sCount--;
}

void nsAutoRollup::SetLastRollup(nsIContent* aLastRollup) {
  MOZ_ASSERT(sCount);

  sLastRollup = aLastRollup;
}

nsIContent* nsAutoRollup::GetLastRollup() { return sLastRollup.get(); }

}  
}  
