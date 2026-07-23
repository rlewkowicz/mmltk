/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef CDMStorageIdProvider_h_
#define CDMStorageIdProvider_h_


#include "nsString.h"

namespace mozilla {

class CDMStorageIdProvider {
  static constexpr const char* kBrowserIdentifier = "mozilla_firefox_gecko";

 public:
  static constexpr int kCurrentVersion = 1;
  static constexpr int kCDMRequestLatestVersion = 0;

  static nsCString ComputeStorageId(const nsCString& aOriginSalt);
};

}  

#endif  // CDMStorageIdProvider_h_
