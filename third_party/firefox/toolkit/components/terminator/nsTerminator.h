/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsTerminator_h_
#define nsTerminator_h_

#include "nsISupports.h"

namespace mozilla {

class nsTerminator final : public nsISupports {
 public:
  NS_DECL_ISUPPORTS

  nsTerminator();
  void AdvancePhase(mozilla::ShutdownPhase aPhase);

 private:
  void Start();
  void StartWatchdog();
  void StartWriter();

  void UpdateHeartbeat(int aStep);
  void UpdateTelemetry();

  ~nsTerminator() = default;

  bool mInitialized;
  int mCurrentStep;
};

}  

#define NS_TOOLKIT_TERMINATOR_CID \
  {0x2e59cc70, 0xf83a, 0x412f, {0x89, 0xd4, 0x45, 0x38, 0x85, 0x83, 0x72, 0x17}}
#define NS_TOOLKIT_TERMINATOR_CONTRACTID \
  "@mozilla.org/toolkit/shutdown-terminator;1"

#endif  // nsTerminator_h_
