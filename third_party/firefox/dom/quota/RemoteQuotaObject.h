/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_REMOTEQUOTAOBJECT_H_
#define DOM_QUOTA_REMOTEQUOTAOBJECT_H_

#include "mozilla/dom/quota/QuotaObject.h"

namespace mozilla::dom::quota {

class RemoteQuotaObjectChild;

class RemoteQuotaObject final : public QuotaObject {
 public:
  explicit RemoteQuotaObject(RefPtr<RemoteQuotaObjectChild> aActor);

  NS_INLINE_DECL_REFCOUNTING_ONEVENTTARGET(RemoteQuotaObject, override)

  void ClearActor();

  void Close();

  const nsAString& Path() const override;

  [[nodiscard]] bool MaybeUpdateSize(int64_t aSize, bool aTruncate) override;

  bool IncreaseSize(int64_t aDelta) override { return false; }

  void DisableQuotaCheck() override {}

  void EnableQuotaCheck() override {}

 private:
  ~RemoteQuotaObject();

  RefPtr<RemoteQuotaObjectChild> mActor;
};

}  

#endif  // DOM_QUOTA_REMOTEQUOTAOBJECT_H_
