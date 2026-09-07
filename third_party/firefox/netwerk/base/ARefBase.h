/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef mozilla_net_ARefBase_h
#define mozilla_net_ARefBase_h

#include "nsISupportsImpl.h"

namespace mozilla {
namespace net {


class ARefBase {
 public:
  ARefBase() = default;
  virtual ~ARefBase() = default;

  NS_INLINE_DECL_PURE_VIRTUAL_REFCOUNTING
};

}  
}  

#endif
