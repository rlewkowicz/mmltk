/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef mozilla_dom_CallbackInterface_h
#define mozilla_dom_CallbackInterface_h

#include "mozilla/dom/CallbackObject.h"

namespace mozilla::dom {

class CallbackInterface : public CallbackObject {
 public:
  explicit CallbackInterface(JSContext* aCx, JS::Handle<JSObject*> aCallback,
                             JS::Handle<JSObject*> aCallbackGlobal,
                             nsIGlobalObject* aIncumbentGlobal)
      : CallbackObject(aCx, aCallback, aCallbackGlobal, aIncumbentGlobal) {}

  explicit CallbackInterface(JSObject* aCallback, JSObject* aCallbackGlobal,
                             JSObject* aAsyncStack,
                             nsIGlobalObject* aIncumbentGlobal)
      : CallbackObject(aCallback, aCallbackGlobal, aAsyncStack,
                       aIncumbentGlobal) {}

 protected:
  bool GetCallableProperty(BindingCallContext& cx, JS::Handle<jsid> aPropId,
                           JS::MutableHandle<JS::Value> aCallable);

  CallbackInterface(JSObject* aCallback, JSObject* aCallbackGlobal,
                    const FastCallbackConstructor&)
      : CallbackObject(aCallback, aCallbackGlobal, FastCallbackConstructor()) {}
};

}  

#endif  // mozilla_dom_CallbackFunction_h
