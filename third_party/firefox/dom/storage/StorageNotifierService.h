/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_StorageNotifierService_h
#define mozilla_dom_StorageNotifierService_h

#include "nsISupportsImpl.h"
#include "nsTObserverArray.h"

class nsIEventTarget;
class nsIPrincipal;
class nsPIDOMWindowInner;

namespace mozilla::dom {

class StorageEvent;

class StorageNotificationObserver {
 public:
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

  virtual void ObserveStorageNotification(StorageEvent* aEvent,
                                          const char16_t* aStorageType,
                                          bool aPrivateBrowsing) = 0;

  virtual bool IsPrivateBrowsing() const = 0;

  virtual nsIPrincipal* GetEffectiveCookiePrincipal() const = 0;

  virtual nsIPrincipal* GetEffectiveStoragePrincipal() const = 0;

  virtual nsIEventTarget* GetEventTarget() const = 0;
};

class StorageNotifierService final {
 public:
  NS_INLINE_DECL_REFCOUNTING(StorageNotifierService)

  static StorageNotifierService* GetOrCreate();

  static void Broadcast(StorageEvent* aEvent, const char16_t* aStorageType,
                        bool aPrivateBrowsing, bool aImmediateDispatch);

  void Register(StorageNotificationObserver* aObserver);

  void Unregister(StorageNotificationObserver* aObserver);

 private:
  StorageNotifierService();
  ~StorageNotifierService();

  nsTObserverArray<RefPtr<StorageNotificationObserver>> mObservers;
};

}  

#endif  // mozilla_dom_StorageNotifierService_h
