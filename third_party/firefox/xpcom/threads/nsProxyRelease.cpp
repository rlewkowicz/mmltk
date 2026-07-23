/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "nsProxyRelease.h"
#include "nsThreadUtils.h"

namespace detail {

 nsresult ProxyReleaseChooser<true>::ProxyReleaseISupports(
    const char* aName, nsIEventTarget* aTarget, nsISupports* aDoomed,
    bool aAlwaysProxy) {
  return ::detail::ProxyRelease<nsISupports>(
      aName, aTarget, dont_AddRef(aDoomed), aAlwaysProxy);
}

}  

extern "C" {

void NS_ProxyReleaseISupports(const char* aName, nsIEventTarget* aTarget,
                              nsISupports* aDoomed, bool aAlwaysProxy) {
  NS_ProxyRelease(aName, aTarget, dont_AddRef(aDoomed), aAlwaysProxy);
}

}  
