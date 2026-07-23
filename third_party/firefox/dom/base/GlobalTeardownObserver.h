/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef DOM_BASE_GLOBALTEARDOWNOBSERVER_H_
#define DOM_BASE_GLOBALTEARDOWNOBSERVER_H_

#include "mozilla/Attributes.h"
#include "nsIGlobalObject.h"
#include "nsIScriptGlobalObject.h"

namespace mozilla {

class GlobalTeardownObserver

    : public nsISupports,
      public LinkedListElement<GlobalTeardownObserver> {
 public:
  GlobalTeardownObserver();
  explicit GlobalTeardownObserver(nsIGlobalObject* aGlobalObject,
                                  bool aHasOrHasHadOwnerWindow = false);

  nsGlobalWindowInner* GetOwnerWindow() const;
  nsIGlobalObject* GetRelevantGlobal() const { return mParentObject; }
  bool HasOrHasHadOwnerWindow() const { return mHasOrHasHadOwnerWindow; }

  void GetParentObject(nsIScriptGlobalObject** aParentObject) {
    if (mParentObject) {
      CallQueryInterface(mParentObject, aParentObject);
    } else {
      *aParentObject = nullptr;
    }
  }

  virtual void DisconnectFromOwner();

  nsresult CheckCurrentGlobalCorrectness() const;

 protected:
  virtual ~GlobalTeardownObserver();

  void BindToGlobal(nsIGlobalObject* aGlobal);

 private:
  nsIGlobalObject* MOZ_NON_OWNING_REF mParentObject = nullptr;
  bool mHasOrHasHadOwnerWindow = false;
};

}  

#endif
