/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_WrapperCallbacks_h
#define js_WrapperCallbacks_h

#include "js/TypeDecls.h"

using JSWrapObjectCallback = JSObject* (*)(JSContext*, JS::HandleObject,
                                           JS::HandleObject);

using JSPreWrapCallback = void (*)(JSContext*, JS::HandleObject,
                                   JS::HandleObject, JS::HandleObject,
                                   JS::HandleObject, JS::MutableHandleObject);

struct JSWrapObjectCallbacks {
  JSWrapObjectCallback wrap;
  JSPreWrapCallback preWrap;
};

#endif  // js_WrapperCallbacks_h
