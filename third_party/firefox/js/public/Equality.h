/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */


#ifndef js_Equality_h
#define js_Equality_h

#include "mozilla/FloatingPoint.h"

#include "jstypes.h"  // JS_PUBLIC_API

#include "js/TypeDecls.h"

struct JS_PUBLIC_API JSContext;

namespace JS {

extern JS_PUBLIC_API bool StrictlyEqual(JSContext* cx, JS::Handle<JS::Value> v1,
                                        JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API bool LooselyEqual(JSContext* cx, JS::Handle<JS::Value> v1,
                                       JS::Handle<JS::Value> v2, bool* equal);

extern JS_PUBLIC_API bool SameValue(JSContext* cx, JS::Handle<JS::Value> v1,
                                    JS::Handle<JS::Value> v2, bool* same);

static inline bool SameValueZero(double v1, double v2) {
  return mozilla::EqualOrBothNaN(v1, v2);
}

}  

#endif /* js_Equality_h */
