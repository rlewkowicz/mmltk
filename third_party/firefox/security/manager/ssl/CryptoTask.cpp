/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CryptoTask.h"
#include "nsNSSComponent.h"
#include "nsNetCID.h"

namespace mozilla {

nsresult CryptoTask::Dispatch() {
  if (!EnsureNSSInitializedChromeOrContent()) {
    return NS_ERROR_FAILURE;
  }

  nsCOMPtr<nsIEventTarget> target(
      do_GetService(NS_STREAMTRANSPORTSERVICE_CONTRACTID));
  if (!target) {
    return NS_ERROR_FAILURE;
  }
  return target->Dispatch(this, NS_DISPATCH_NORMAL);
}

NS_IMETHODIMP
CryptoTask::Run() {
  if (!NS_IsMainThread()) {
    mRv = CalculateResult();
    NS_DispatchToMainThread(this);
  } else {
    CallCallback(mRv);
  }
  return NS_OK;
}

}  
