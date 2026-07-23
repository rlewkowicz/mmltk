/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ServiceWorkerUnregisterJob.h"

#include "ServiceWorkerManager.h"
#include "mozilla/dom/CookieStoreSubscriptionService.h"
#include "nsThreadUtils.h"

namespace mozilla::dom {

ServiceWorkerUnregisterJob::ServiceWorkerUnregisterJob(nsIPrincipal* aPrincipal,
                                                       const nsACString& aScope)
    : ServiceWorkerJob(Type::Unregister, aPrincipal, aScope, ""_ns),
      mResult(false) {}

bool ServiceWorkerUnregisterJob::GetResult() const {
  MOZ_ASSERT(NS_IsMainThread());
  return mResult;
}

ServiceWorkerUnregisterJob::~ServiceWorkerUnregisterJob() = default;

void ServiceWorkerUnregisterJob::AsyncExecute() {
  MOZ_ASSERT(NS_IsMainThread());

  if (Canceled()) {
    Finish(NS_ERROR_DOM_ABORT_ERR);
    return;
  }

  CookieStoreSubscriptionService::ServiceWorkerUnregistered(mPrincipal, mScope);
  Unregister();
}

void ServiceWorkerUnregisterJob::Unregister() {
  MOZ_ASSERT(NS_IsMainThread());

  RefPtr<ServiceWorkerManager> swm = ServiceWorkerManager::GetInstance();
  if (Canceled() || !swm) {
    Finish(NS_ERROR_DOM_ABORT_ERR);
    return;
  }


  RefPtr<ServiceWorkerRegistrationInfo> registration =
      swm->GetRegistration(mPrincipal, mScope);
  if (!registration) {
    Finish(NS_OK);
    return;
  }

  swm->MaybeSendUnregister(mPrincipal, mScope);

  swm->EvictFromBFCache(registration);

  swm->RemoveRegistration(registration);
  MOZ_ASSERT(registration->IsUnregistered());

  mResult = true;
  InvokeResultCallbacks(NS_OK);

  if (!registration->IsControllingClients()) {
    if (registration->IsIdle()) {
      registration->Clear();
    } else {
      registration->ClearWhenIdle();
    }
  }

  Finish(NS_OK);
}

}  
