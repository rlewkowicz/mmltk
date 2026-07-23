/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_serviceworkerregisterjob_h
#define mozilla_dom_serviceworkerregisterjob_h

#include "ServiceWorkerUpdateJob.h"

namespace mozilla::dom {

class ServiceWorkerRegisterJob final : public ServiceWorkerUpdateJob {
 public:
  ServiceWorkerRegisterJob(
      nsIPrincipal* aPrincipal, const nsACString& aScope,
      const WorkerType& aType, const nsACString& aScriptSpec,
      ServiceWorkerUpdateViaCache aUpdateViaCache,
      const ServiceWorkerLifetimeExtension& aLifetimeExtension,
      uint16_t aIPAddressSpace = 0);

 private:
  WorkerType mType;
  uint16_t mIPAddressSpace;

  virtual void AsyncExecute() override;

  virtual ~ServiceWorkerRegisterJob();
};

}  

#endif  // mozilla_dom_serviceworkerregisterjob_h
