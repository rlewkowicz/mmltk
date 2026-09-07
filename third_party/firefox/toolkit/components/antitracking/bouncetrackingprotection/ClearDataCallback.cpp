/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "ClearDataCallback.h"

using namespace mozilla;

NS_IMPL_ISUPPORTS(ClearDataCallback, nsIClearDataCallback);

ClearDataCallback::ClearDataCallback(ClearDataMozPromise::Private* aPromise,
                                     const OriginAttributes& aOriginAttributes,
                                     const nsACString& aHost,
                                     PRTime aBounceTime,
                                     BounceTrackingRecord* aChainRecord)
    : mPromise(aPromise) {
  MOZ_ASSERT(!aHost.IsEmpty(), "Host must not be empty");

  mEntry =
      new BounceTrackingPurgeEntry(aOriginAttributes, aHost, aBounceTime, 0);
  if (aChainRecord) {
    mEntry->SetBounceChainRecord(aChainRecord);
  }
}

ClearDataCallback::~ClearDataCallback() { mPromise->Reject(0, __func__); }

NS_IMETHODIMP ClearDataCallback::OnDataDeleted(uint32_t aFailedFlags) {
  if (aFailedFlags) {
    mPromise->Reject(aFailedFlags, __func__);
  } else {
    MOZ_LOG_FMT(gBounceTrackingProtectionLog, LogLevel::Debug,
                "{}: Cleared host: {}, bounceTime: {}", __FUNCTION__,
                mEntry->SiteHostRef(), mEntry->TimeStampRef());

    mEntry->PurgeTimeRef() = PR_Now();
    mPromise->Resolve(mEntry, __func__);
  }

  return NS_OK;
}
