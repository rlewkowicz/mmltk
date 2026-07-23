/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_serviceworkerunregisterjob_h
#define mozilla_dom_serviceworkerunregisterjob_h

#include "ServiceWorkerJob.h"

namespace mozilla {
template <typename ResolveValueT, typename RejectValueT, bool IsExclusive>
class MozPromise;
using GenericPromise = MozPromise<bool, nsresult,  true>;
}  

namespace mozilla::dom {

class ServiceWorkerUnregisterJob final : public ServiceWorkerJob {
 public:
  ServiceWorkerUnregisterJob(nsIPrincipal* aPrincipal,
                             const nsACString& aScope);

  bool GetResult() const;

 private:
  virtual ~ServiceWorkerUnregisterJob();

  void AsyncExecute() override;

  void Unregister();

  bool mResult;
};

}  

#endif  // mozilla_dom_serviceworkerunregisterjob_h
