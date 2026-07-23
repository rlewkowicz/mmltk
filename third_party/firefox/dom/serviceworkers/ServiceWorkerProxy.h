/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef moz_dom_ServiceWorkerProxy_h
#define moz_dom_ServiceWorkerProxy_h

#include "ServiceWorkerDescriptor.h"
#include "nsProxyRelease.h"

namespace mozilla::dom {

class PostMessageSource;
class ServiceWorkerInfo;
class ServiceWorkerParent;

namespace ipc {
class StructuredCloneData;
}

class ServiceWorkerProxy final {
  RefPtr<ServiceWorkerParent> mActor;

  nsCOMPtr<nsISerialEventTarget> mEventTarget;

  ServiceWorkerDescriptor mDescriptor;
  nsMainThreadPtrHandle<ServiceWorkerInfo> mInfo;

  ~ServiceWorkerProxy();

  void MaybeShutdownOnBGThread();

  void SetStateOnBGThread(ServiceWorkerState aState);

  void InitOnMainThread();

  void MaybeShutdownOnMainThread();

  void StopListeningOnMainThread();

 public:
  explicit ServiceWorkerProxy(const ServiceWorkerDescriptor& aDescriptor);

  void Init(ServiceWorkerParent* aActor);

  void RevokeActor(ServiceWorkerParent* aActor);

  void PostMessage(ipc::StructuredCloneData* aData,
                   const PostMessageSource& aSource);

  NS_INLINE_DECL_THREADSAFE_REFCOUNTING(ServiceWorkerProxy);
};

}  

#endif  // moz_dom_ServiceWorkerProxy_h
