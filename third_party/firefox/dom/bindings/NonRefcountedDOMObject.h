/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_NonRefcountedDOMObject_h_
#define mozilla_dom_NonRefcountedDOMObject_h_

#include "nsISupportsImpl.h"

namespace mozilla::dom {

class NonRefcountedDOMObject {
 protected:
  MOZ_COUNTED_DEFAULT_CTOR(NonRefcountedDOMObject)

  MOZ_COUNTED_DTOR(NonRefcountedDOMObject)

  NonRefcountedDOMObject(const NonRefcountedDOMObject& aOther)
      : NonRefcountedDOMObject() {}

  NonRefcountedDOMObject& operator=(const NonRefcountedDOMObject& aOther) {
    NonRefcountedDOMObject();
    return *this;
  }
};

}  

#endif /* mozilla_dom_NonRefcountedDOMObject_h_ */
