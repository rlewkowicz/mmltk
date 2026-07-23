/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_MozQueryInterface
#define mozilla_dom_MozQueryInterface


#include "mozilla/dom/BindingDeclarations.h"
#include "mozilla/dom/NonRefcountedDOMObject.h"
#include "nsID.h"

namespace mozilla {
class ErrorResult;

namespace dom {

class MozQueryInterface final : public NonRefcountedDOMObject {
 public:
  explicit MozQueryInterface(nsTArray<nsIID>&& aInterfaces)
      : mInterfaces(std::move(aInterfaces)) {}

  bool QueriesTo(const nsIID& aIID) const;

  void LegacyCall(JSContext* cx, JS::Handle<JS::Value> thisv,
                  JS::Handle<JS::Value> aIID,
                  JS::MutableHandle<JS::Value> aResult, ErrorResult& aRv) const;

  nsISupports* GetParentObject() const { return nullptr; }

  bool WrapObject(JSContext* aCx, JS::Handle<JSObject*> aGivenProto,
                  JS::MutableHandle<JSObject*> aReflector);

 private:
  nsTArray<nsIID> mInterfaces;
};

}  
}  

#endif  // mozilla_dom_MozQueryInterface
