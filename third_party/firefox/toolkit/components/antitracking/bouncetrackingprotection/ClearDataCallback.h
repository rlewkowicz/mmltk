/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef mozilla_ClearDataCallback_h_
#define mozilla_ClearDataCallback_h_

#include "BounceTrackingMapEntry.h"
#include "BounceTrackingRecord.h"
#include "mozilla/MozPromise.h"
#include "nsIClearDataService.h"

namespace mozilla {

using ClearDataMozPromise =
    MozPromise<RefPtr<BounceTrackingPurgeEntry>, uint32_t, true>;

extern LazyLogModule gBounceTrackingProtectionLog;

class ClearDataCallback final : public nsIClearDataCallback {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSICLEARDATACALLBACK

  explicit ClearDataCallback(ClearDataMozPromise::Private* aPromise,
                             const OriginAttributes& aOriginAttributes,
                             const nsACString& aHost, PRTime aBounceTime,
                             BounceTrackingRecord* aChainRecord = nullptr);

 private:
  virtual ~ClearDataCallback();

  RefPtr<BounceTrackingPurgeEntry> mEntry;

  RefPtr<ClearDataMozPromise::Private> mPromise;

};

}  

#endif
