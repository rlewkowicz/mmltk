/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef XPC_WRAPPER_H
#define XPC_WRAPPER_H 1

#include "js/TypeDecls.h"

namespace XPCNativeWrapper {

bool AttachNewConstructorObject(JSContext* aCx, JS::HandleObject aGlobalObject);

}  

namespace XPCWrapper {

JSObject* UnsafeUnwrapSecurityWrapper(JSObject* obj);

}  

#endif
