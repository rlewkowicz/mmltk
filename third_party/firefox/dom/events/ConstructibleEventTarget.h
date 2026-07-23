/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ConstructibleEventTarget_h_
#define mozilla_dom_ConstructibleEventTarget_h_

#include "js/RootingAPI.h"
#include "mozilla/DOMEventTargetHelper.h"

namespace mozilla::dom {

class ConstructibleEventTarget : public DOMEventTargetHelper {
 public:

  explicit ConstructibleEventTarget(nsIGlobalObject* aGlobalObject)
      : DOMEventTargetHelper(aGlobalObject) {}

  virtual JSObject* WrapObject(JSContext* cx,
                               JS::Handle<JSObject*> aGivenProto) override;
};

}  

#endif  // mozilla_dom_ConstructibleEventTarget_h_
