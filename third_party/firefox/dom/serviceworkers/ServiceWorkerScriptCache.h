/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ServiceWorkerScriptCache_h
#define mozilla_dom_ServiceWorkerScriptCache_h

#include "nsIRequest.h"
#include "nsISupportsImpl.h"
#include "nsString.h"

class nsILoadGroup;
class nsIPrincipal;

namespace mozilla::dom {

class ServiceWorkerRegistrationInfo;

namespace serviceWorkerScriptCache {

nsresult PurgeCache(nsIPrincipal* aPrincipal, const nsAString& aCacheName);

nsresult GenerateCacheName(nsAString& aName);

enum class OnFailure : uint8_t { DoNothing, Uninstall };

class CompareCallback {
 public:
  virtual void ComparisonResult(nsresult aStatus, bool aInCacheAndEqual,
                                OnFailure aOnFailure,
                                const nsAString& aNewCacheName,
                                const nsACString& aMaxScope,
                                nsLoadFlags aLoadFlags) = 0;

  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING
};

nsresult Compare(ServiceWorkerRegistrationInfo* aRegistration,
                 nsIPrincipal* aPrincipal, const nsAString& aCacheName,
                 const nsACString& aURL, CompareCallback* aCallback);

}  

}  

#endif  // mozilla_dom_ServiceWorkerScriptCache_h
