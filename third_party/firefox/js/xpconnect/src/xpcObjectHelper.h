/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(xpcObjectHelper_h)
#define xpcObjectHelper_h


#include "mozilla/Attributes.h"
#include <stdint.h>
#include "nsCOMPtr.h"
#include "nsIClassInfo.h"
#include "nsISupports.h"
#include "nsIXPCScriptable.h"
#include "nsWrapperCache.h"

class xpcObjectHelper {
 public:
  explicit xpcObjectHelper(nsISupports* aObject,
                           nsWrapperCache* aCache = nullptr)
      : mObject(aObject), mCache(aCache) {
    if (!mCache && aObject) {
      CallQueryInterface(aObject, &mCache);
    }
  }

  nsISupports* Object() { return mObject; }

  nsIClassInfo* GetClassInfo() {
    if (!mClassInfo) {
      mClassInfo = do_QueryInterface(mObject);
    }
    return mClassInfo;
  }

  uint32_t GetScriptableFlags() {
    nsCOMPtr<nsIXPCScriptable> sinfo = do_QueryInterface(mObject);

    MOZ_ASSERT(sinfo);

    return sinfo->GetScriptableFlags();
  }

  nsWrapperCache* GetWrapperCache() { return mCache; }

 private:
  xpcObjectHelper(xpcObjectHelper& aOther) = delete;

  nsISupports* MOZ_UNSAFE_REF(
      "xpcObjectHelper has been specifically optimized "
      "to avoid unnecessary AddRefs and Releases. "
      "(see bug 565742)") mObject;
  nsWrapperCache* mCache;
  nsCOMPtr<nsIClassInfo> mClassInfo;
};

#endif
