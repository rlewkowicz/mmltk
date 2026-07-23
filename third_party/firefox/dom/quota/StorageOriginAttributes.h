/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_QUOTA_STORAGEORIGINATTRIBUTES_H_
#define DOM_QUOTA_STORAGEORIGINATTRIBUTES_H_

#include "mozilla/OriginAttributes.h"

namespace mozilla {

class StorageOriginAttributes {
 public:
  StorageOriginAttributes() : mInIsolatedMozBrowser(false) {}

  explicit StorageOriginAttributes(bool aInIsolatedMozBrowser)
      : mInIsolatedMozBrowser(aInIsolatedMozBrowser) {}

  bool InIsolatedMozBrowser() const { return mInIsolatedMozBrowser; }

  uint32_t UserContextId() const { return mOriginAttributes.mUserContextId; }


  void SetInIsolatedMozBrowser(bool aInIsolatedMozBrowser) {
    mInIsolatedMozBrowser = aInIsolatedMozBrowser;
  }

  void SetUserContextId(uint32_t aUserContextId) {
    mOriginAttributes.mUserContextId = aUserContextId;
  }


  void CreateSuffix(nsACString& aStr) const;

  [[nodiscard]] bool PopulateFromSuffix(const nsACString& aStr);

  [[nodiscard]] bool PopulateFromOrigin(const nsACString& aOrigin,
                                        nsACString& aOriginNoSuffix);

 private:
  OriginAttributes mOriginAttributes;

  bool mInIsolatedMozBrowser;
};

}  

#endif  // DOM_QUOTA_STORAGEORIGINATTRIBUTES_H_
