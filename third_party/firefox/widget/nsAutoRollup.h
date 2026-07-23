/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsAutoRollup_h_
#define nsAutoRollup_h_

#include "mozilla/Attributes.h"  // for MOZ_RAII
#include "mozilla/StaticPtr.h"   // for StaticRefPtr
#include "nsIContent.h"

namespace mozilla {
namespace widget {

class MOZ_RAII nsAutoRollup {
 public:
  nsAutoRollup();
  ~nsAutoRollup();

  explicit nsAutoRollup(nsIContent* aRollup);

  static void SetLastRollup(nsIContent* aLastRollup);
  static nsIContent* GetLastRollup();

 private:
  bool mWasClear;

  static uint32_t sCount;
  static StaticRefPtr<nsIContent> sLastRollup;
};

}  
}  

#endif  // nsAutoRollup_h_
