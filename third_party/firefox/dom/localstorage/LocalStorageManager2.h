/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_localstorage_LocalStorageManager2_h
#define mozilla_dom_localstorage_LocalStorageManager2_h

#include "ErrorList.h"
#include "nsIDOMStorageManager.h"
#include "nsILocalStorageManager.h"
#include "nsISupports.h"

namespace mozilla::dom {

class LSRequestChild;
class LSRequestChildCallback;
class LSRequestParams;
class LSSimpleRequestParams;
class Promise;

class LocalStorageManager2 final : public nsIDOMStorageManager,
                                   public nsILocalStorageManager {
 public:
  LocalStorageManager2();

  NS_DECL_ISUPPORTS
  NS_DECL_NSIDOMSTORAGEMANAGER
  NS_DECL_NSILOCALSTORAGEMANAGER

  LSRequestChild* StartRequest(const LSRequestParams& aParams,
                               LSRequestChildCallback* aCallback);

 private:
  ~LocalStorageManager2();

  nsresult StartSimpleRequest(Promise* aPromise,
                              const LSSimpleRequestParams& aParams);
};

}  

#endif  // mozilla_dom_localstorage_LocalStorageManager2_h
