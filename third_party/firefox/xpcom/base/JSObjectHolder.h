/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_JSObjectHolder_h
#define mozilla_JSObjectHolder_h

#include "js/RootingAPI.h"
#include "nsISupportsImpl.h"

namespace mozilla {

class JSObjectHolder final : public nsISupports {
 public:
  JSObjectHolder(JSContext* aCx, JSObject* aObject) : mJSObject(aCx, aObject) {}

  NS_DECL_ISUPPORTS

  JSObject* GetJSObject() { return mJSObject; }

 private:
  ~JSObjectHolder() = default;

  JS::PersistentRooted<JSObject*> mJSObject;
};

}  

#endif  // mozilla_JSObjectHolder_h
