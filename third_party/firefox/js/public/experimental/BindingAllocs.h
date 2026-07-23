/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_experimental_BindingAllocs_h
#define js_experimental_BindingAllocs_h

#include "jstypes.h"

extern JS_PUBLIC_API JSObject* JS_NewObjectWithGivenProtoAndUseAllocSite(
    JSContext* cx, const JSClass* clasp, JS::Handle<JSObject*> proto);

#endif  // js_experimental_BindingAllocs_h
