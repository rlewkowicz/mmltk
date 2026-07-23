/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef nsOSPermissionRequestBase_h_
#define nsOSPermissionRequestBase_h_

#include "nsIOSPermissionRequest.h"
#include "nsWeakReference.h"

namespace mozilla::dom {
class Promise;
}  

using mozilla::dom::Promise;

class nsOSPermissionRequestBase : public nsIOSPermissionRequest,
                                  public nsSupportsWeakReference {
 public:
  NS_DECL_ISUPPORTS
  NS_DECL_NSIOSPERMISSIONREQUEST

  nsOSPermissionRequestBase() = default;

 protected:
  nsresult GetPromise(JSContext* aCx, RefPtr<Promise>& aPromiseOut);
  virtual ~nsOSPermissionRequestBase() = default;
};

#endif
