/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef nsIReflowCallback_h_
#define nsIReflowCallback_h_

class nsIReflowCallback {
 public:
  virtual bool ReflowFinished() = 0;
  virtual void ReflowCallbackCanceled() = 0;
};

#endif /* nsIReflowCallback_h_ */
