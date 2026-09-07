/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef moz_dom_ServiceWorkerRegistrationProxy_h
#define moz_dom_ServiceWorkerRegistrationProxy_h

#include "ServiceWorkerRegistrationDescriptor.h"
#include "ServiceWorkerRegistrationListener.h"
#include "ServiceWorkerUtils.h"
#include "mozilla/dom/ClientInfo.h"
#include "mozilla/dom/PServiceWorkerRegistrationParent.h"
#include "nsProxyRelease.h"

namespace mozilla::dom {

class ServiceWorkerRegistrationInfo;
class ServiceWorkerRegistrationParent;

class ServiceWorkerRegistrationProxy final
    : public ServiceWorkerRegistrationListener {
  RefPtr<ServiceWorkerRegistrationParent> mActor;

  nsCOMPtr<nsISerialEventTarget> mEventTarget;

  ServiceWorkerRegistrationDescriptor mDescriptor;
  ClientInfo mListeningClientInfo;
  nsMainThreadPtrHandle<ServiceWorkerRegistrationInfo> mReg;

  ~ServiceWorkerRegistrationProxy();

  void MaybeShutdownOnBGThread();

  void UpdateStateOnBGThread(
      const ServiceWorkerRegistrationDescriptor& aDescriptor);

  void FireUpdateFoundOnBGThread();

  void InitOnMainThread();

  void MaybeShutdownOnMainThread();

  void StopListeningOnMainThread();

  class DelayedUpdate;
  RefPtr<DelayedUpdate> mDelayedUpdate;

  void UpdateState(
      const ServiceWorkerRegistrationDescriptor& aDescriptor) override;

  void FireUpdateFound() override;

  void RegistrationCleared() override;

  void GetScope(nsAString& aScope) const override;

  bool MatchesDescriptor(
      const ServiceWorkerRegistrationDescriptor& aDescriptor) override;

 public:
  ServiceWorkerRegistrationProxy(
      const ServiceWorkerRegistrationDescriptor& aDescriptor,
      const ClientInfo& aForClient);

  void Init(ServiceWorkerRegistrationParent* aActor);

  void RevokeActor(ServiceWorkerRegistrationParent* aActor);

  RefPtr<GenericPromise> Unregister();

  RefPtr<ServiceWorkerRegistrationPromise> Update(
      const nsACString& aNewestWorkerScriptUrl);

  RefPtr<GenericPromise> SetNavigationPreloadEnabled(const bool& aEnabled);

  RefPtr<GenericPromise> SetNavigationPreloadHeader(const nsACString& aHeader);

  RefPtr<NavigationPreloadStatePromise> GetNavigationPreloadState();

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ServiceWorkerRegistrationProxy,
                                        override);
};

}  

#endif  // moz_dom_ServiceWorkerRegistrationProxy_h
