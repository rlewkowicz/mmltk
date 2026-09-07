/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_dom_ConsoleCommon_h
#define mozilla_dom_ConsoleCommon_h

#include "jsapi.h"
#include "nsString.h"

namespace mozilla::dom::ConsoleCommon {

class MOZ_RAII ClearException {
 public:
  explicit ClearException(JSContext* aCx) : mCx(aCx) {}

  ~ClearException() { JS_ClearPendingException(mCx); }

 private:
  JSContext* mCx;
};

}  

#endif /* mozilla_dom_ConsoleCommon_h */
