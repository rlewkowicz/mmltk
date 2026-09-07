/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_friend_XrayJitInfo_h
#define js_friend_XrayJitInfo_h

#include <stddef.h>  // size_t

#include "jstypes.h"  // JS_PUBLIC_API

class JS_PUBLIC_API JSObject;

namespace js {

class JS_PUBLIC_API BaseProxyHandler;

}  

namespace JS {

struct XrayJitInfo {
  bool (*isCrossCompartmentXray)(const js::BaseProxyHandler* handler);

  bool (*compartmentHasExclusiveExpandos)(JSObject* obj);

  size_t xrayHolderSlot;

  size_t holderExpandoSlot;

  size_t expandoProtoSlot;
};

extern JS_PUBLIC_API void SetXrayJitInfo(XrayJitInfo* info);

}  

#endif  // js_friend_XrayJitInfo_h
