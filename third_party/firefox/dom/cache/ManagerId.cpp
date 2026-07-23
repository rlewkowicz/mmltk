/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "mozilla/dom/cache/ManagerId.h"

#include "CacheCommon.h"
#include "mozilla/dom/quota/PrincipalUtils.h"
#include "nsIPrincipal.h"
#include "nsProxyRelease.h"
#include "nsThreadUtils.h"

namespace mozilla::dom::cache {

using mozilla::dom::quota::QuotaManager;

Result<SafeRefPtr<ManagerId>, nsresult> ManagerId::Create(
    nsIPrincipal* aPrincipal) {
  MOZ_ASSERT(NS_IsMainThread());

  QM_TRY_INSPECT(const auto& quotaOrigin,
                 quota::GetOriginFromPrincipal(aPrincipal));

  return MakeSafeRefPtr<ManagerId>(aPrincipal, quotaOrigin, ConstructorGuard{});
}

already_AddRefed<nsIPrincipal> ManagerId::Principal() const {
  MOZ_ASSERT(NS_IsMainThread());
  nsCOMPtr<nsIPrincipal> ref = mPrincipal;
  return ref.forget();
}

ManagerId::ManagerId(nsIPrincipal* aPrincipal, const nsACString& aQuotaOrigin,
                     ConstructorGuard)
    : mPrincipal(aPrincipal), mQuotaOrigin(aQuotaOrigin) {
  MOZ_DIAGNOSTIC_ASSERT(mPrincipal);
}

ManagerId::~ManagerId() {
  if (NS_IsMainThread()) {
    return;
  }


  NS_ReleaseOnMainThread("ManagerId::mPrincipal", mPrincipal.forget());
}

}  
