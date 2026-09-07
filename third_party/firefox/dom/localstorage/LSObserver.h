/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_localstorage_LSObserver_h
#define mozilla_dom_localstorage_LSObserver_h

#include "mozilla/Assertions.h"
#include "nsISupports.h"
#include "nsString.h"

namespace mozilla::dom {

class LSObserverChild;

class LSObserver final {
  friend class LSObject;

  LSObserverChild* mActor;

  const nsCString mOrigin;

 public:
  static LSObserver* Get(const nsACString& aOrigin);

  NS_INLINE_DECL_REFCOUNTING(LSObserver)

  void AssertIsOnOwningThread() const { NS_ASSERT_OWNINGTHREAD(LSObserver); }

  void SetActor(LSObserverChild* aActor);

  void ClearActor() {
    AssertIsOnOwningThread();
    MOZ_ASSERT(mActor);

    mActor = nullptr;
  }

 private:
  explicit LSObserver(const nsACString& aOrigin);

  ~LSObserver();
};

}  

#endif  // mozilla_dom_localstorage_LSObserver_h
