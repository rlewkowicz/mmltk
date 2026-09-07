/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#if !defined(mozilla_ipc_LaunchError_h)
#define mozilla_ipc_LaunchError_h

#include "mozilla/StaticString.h"
#include "nsError.h"  // for nsresult


namespace mozilla::ipc {

class LaunchError {
 public:
  using ErrorType = long;

  explicit LaunchError(StaticString aFunction, ErrorType aError = 0)
      : mFunction(aFunction), mError(aError) {}


  LaunchError(StaticString aFunction, nsresult aError)
      : mFunction(aFunction), mError((ErrorType)aError) {}

  StaticString FunctionName() const { return mFunction; }
  ErrorType ErrorCode() const { return mError; }

 private:
  StaticString mFunction;
  ErrorType mError;
};

}  

#endif
