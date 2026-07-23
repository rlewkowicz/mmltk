/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_power_WakeLock_h
#define mozilla_dom_power_WakeLock_h

#include "nsCOMPtr.h"
#include "nsIDOMEventListener.h"
#include "nsIWakeLock.h"
#include "nsString.h"
#include "nsWeakReference.h"
#include "nsWrapperCache.h"

class nsPIDOMWindowInner;

namespace mozilla {
class ErrorResult;

namespace dom {
class Document;

class WakeLock final : public nsIDOMEventListener,
                       public nsSupportsWeakReference,
                       public nsIWakeLock {
 public:
  NS_DECL_NSIDOMEVENTLISTENER
  NS_DECL_NSIWAKELOCK

  NS_DECL_ISUPPORTS


  WakeLock() = default;

  nsresult Init(const nsAString& aTopic, nsPIDOMWindowInner* aWindow);


  nsPIDOMWindowInner* GetParentObject() const;

  void GetTopic(nsAString& aTopic);

  void Unlock(ErrorResult& aRv);

 private:
  virtual ~WakeLock();

  void DoUnlock();
  void DoLock();
  void AttachEventListener();
  void DetachEventListener();

  bool mLocked = false;
  bool mHidden = true;

  nsString mTopic;

  nsWeakPtr mWindow;
};

}  
}  

#endif  // mozilla_dom_power_WakeLock_h
