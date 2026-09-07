/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef js_experimental_Intl_h
#define js_experimental_Intl_h

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

namespace JS {

extern JS_PUBLIC_API bool AddMozDateTimeFormatConstructor(
    JSContext* cx, Handle<JSObject*> intl);

extern JS_PUBLIC_API bool AddMozDisplayNamesConstructor(JSContext* cx,
                                                        Handle<JSObject*> intl);

extern JS_PUBLIC_API bool AddMozGetCalendarInfo(JSContext* cx,
                                                Handle<JSObject*> intl);

}  

#endif  // js_experimental_Intl_h
