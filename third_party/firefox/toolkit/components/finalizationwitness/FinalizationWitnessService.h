/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_finalizationwitnessservice_h_
#define mozilla_finalizationwitnessservice_h_

#include "nsIFinalizationWitnessService.h"
#include "nsIObserver.h"

namespace mozilla {

class FinalizationWitnessService final : public nsIFinalizationWitnessService,
                                         public nsIObserver {
 public:
  void operator=(const FinalizationWitnessService* other) = delete;

  NS_DECL_ISUPPORTS
  NS_DECL_NSIFINALIZATIONWITNESSSERVICE
  NS_DECL_NSIOBSERVER

  nsresult Init();

 private:
  ~FinalizationWitnessService() = default;
};

}  

#endif  // mozilla_finalizationwitnessservice_h_
