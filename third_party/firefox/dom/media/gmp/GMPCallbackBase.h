/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef GMPCallbackBase_h_
#define GMPCallbackBase_h_

#include "nsISupportsImpl.h"

class GMPCallbackBase {
 public:
  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING

  virtual ~GMPCallbackBase() = default;

  virtual void Terminated() = 0;
};

#endif
