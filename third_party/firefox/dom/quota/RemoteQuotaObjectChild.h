/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_REMOTEQUOTAOBJECTCHILD_H_
#define DOM_QUOTA_REMOTEQUOTAOBJECTCHILD_H_

#include "mozilla/dom/quota/PRemoteQuotaObjectChild.h"

namespace mozilla::dom::quota {

class RemoteQuotaObject;

class RemoteQuotaObjectChild : public PRemoteQuotaObjectChild {
 public:
  RemoteQuotaObjectChild();

  NS_INLINE_DECL_REFCOUNTING_ONEVENTTARGET(RemoteQuotaObjectChild, override)

  void SetRemoteQuotaObject(RemoteQuotaObject* aRemoteQuotaObject);

  void ActorDestroy(ActorDestroyReason aWhy) override;

 private:
  virtual ~RemoteQuotaObjectChild();

  RemoteQuotaObject* MOZ_NON_OWNING_REF mRemoteQuotaObject;
};

}  

#endif  // DOM_QUOTA_REMOTEQUOTAOBJECTCHILD_H_
